#!/usr/bin/env python3
"""Run deterministic CPU, GPU, OCIO, and full-pipeline tonemapping validation."""

from __future__ import annotations

import argparse
import array
import html
import json
import math
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
METHODS = ("gt7", "agx", "aces", "neutral")
OUTPUTS = ("sdr", "edr", "hdr10")
CPU_TESTS = (
    "gt7_tonemapping_tests",
    "aces2_tonemapping_tests",
    "agx_tonemapping_tests",
    "pbr_neutral_tonemapping_tests",
    "display_encoding_tests",
    "output_selection_tests",
)


def run(command: list[str], *, env: dict[str, str] | None = None,
        output: Path | None = None) -> None:
    print("+", " ".join(command), flush=True)
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("w", encoding="utf-8") as log:
            subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    else:
        subprocess.run(command, cwd=ROOT, env=env, check=True)


def compiler() -> str:
    candidate = shutil.which("clang++") or shutil.which("c++")
    if not candidate:
        raise RuntimeError("clang++ or c++ is required")
    return candidate


def vulkan_include_args() -> list[str]:
    sdk = os.environ.get("VULKAN_SDK")
    if sdk and (Path(sdk) / "include" / "vulkan" / "vulkan.h").is_file():
        return ["-I", str(Path(sdk) / "include")]
    return []


def configure_moltenvk(environment: dict[str, str]) -> None:
    if platform.system() != "Darwin" or environment.get("VK_ICD_FILENAMES"):
        return
    sdk = os.environ.get("VULKAN_SDK", "")
    candidates = (
        Path(sdk) / "share/vulkan/icd.d/MoltenVK_icd.json" if sdk else Path("/__missing__"),
        Path("/usr/local/share/vulkan/icd.d/MoltenVK_icd.json"),
        Path("/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"),
    )
    for candidate in candidates:
        if candidate.is_file():
            environment["VK_ICD_FILENAMES"] = str(candidate)
            return


def compile_cpu_tests(binary_dir: Path) -> None:
    cxx = compiler()
    common = [cxx, "-std=c++20", "-O2", "-I", "src", "-I", "extern",
              "-I", "data/shaders"]
    for name in CPU_TESTS:
        run(common + [f"tests/{name}.cpp", "-o", str(binary_dir / name)])
    run(common + ["tests/tonemapping_conformance_tests.cpp", "-o",
                  str(binary_dir / "tonemapping_conformance_tests")])


def compile_gpu_test(binary_dir: Path) -> None:
    cxx = compiler()
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        raise RuntimeError("clang or cc is required for Volk")
    volk_object = binary_dir / "volk.o"
    run([cc, "-std=c11", "-O2", "-I", "extern", *vulkan_include_args(),
         "-c", "extern/volk/volk.c", "-o", str(volk_object)])
    run([cxx, "-std=c++20", "-O2", "tests/tonemapping_gpu_validation.cpp",
         str(volk_object), "-I", "src", "-I", "extern", "-I", "data/shaders",
         *vulkan_include_args(), "-o", str(binary_dir / "tonemapping_gpu_validation")])


def run_cpu_suite(binary_dir: Path, report_dir: Path) -> dict:
    for name in CPU_TESTS:
        run([str(binary_dir / name)])
    report = report_dir / "cpu_conformance.json"
    run([str(binary_dir / "tonemapping_conformance_tests"), "--json", str(report)])
    return json.loads(report.read_text(encoding="utf-8"))


def run_gpu_suite(binary_dir: Path, report_dir: Path) -> dict:
    environment = os.environ.copy()
    configure_moltenvk(environment)
    report = report_dir / "gpu_conformance.json"
    run([str(binary_dir / "tonemapping_gpu_validation"), "--json", str(report)],
        env=environment)
    return json.loads(report.read_text(encoding="utf-8"))


def verify_ocio_regeneration(python: str, report_dir: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="game2-tonemapping-ocio-") as temporary:
        generated = Path(temporary)
        for generator, files in (
            ("generate_aces2_luts.py",
             ("aces2_sdr.lutbin", "aces2_edr.lutbin", "aces2_hdr10.lutbin",
              "aces2_manifest.json")),
            ("generate_agx_luts.py",
             ("agx_sdr.lutbin", "agx_hdr1000.lutbin", "agx_manifest.json")),
        ):
            target = generated / generator.removesuffix(".py")
            run([python, f"tools/{generator}", "--output-dir", str(target)])
            for filename in files:
                checked_in = ROOT / "data/tonemapping" / filename
                regenerated = target / filename
                if checked_in.read_bytes() != regenerated.read_bytes():
                    raise RuntimeError(
                        f"non-deterministic or stale asset {filename}; regenerate it with {generator}")
    result = {
        "suite": "tonemapping-ocio-regeneration-v1",
        "passed": True,
        "aces2_manifest": json.loads(
            (ROOT / "data/tonemapping/aces2_manifest.json").read_text(encoding="utf-8")),
        "agx_manifest": json.loads(
            (ROOT / "data/tonemapping/agx_manifest.json").read_text(encoding="utf-8")),
    }
    (report_dir / "ocio_regeneration.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def read_pfm(path: Path) -> tuple[int, int, list[float]]:
    with path.open("rb") as file:
        if file.readline().strip() != b"PF":
            raise RuntimeError(f"{path} is not an RGB PFM")
        width, height = (int(value) for value in file.readline().split())
        scale = float(file.readline())
        values = array.array("f")
        values.frombytes(file.read())
    if scale >= 0.0:
        values.byteswap()
    if len(values) != width * height * 3:
        raise RuntimeError(f"{path} has an invalid payload")
    top_down = [0.0] * len(values)
    row_values = width * 3
    for y in range(height):
        source = (height - 1 - y) * row_values
        top_down[y * row_values:(y + 1) * row_values] = values[source:source + row_values]
    return width, height, top_down


def linear_to_srgb(value: float) -> float:
    value = min(max(value, 0.0), 1.0)
    return value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055


def write_preview(path: Path, width: int, height: int, pixels: list[float]) -> None:
    data = bytearray(width * height * 3)
    for index, value in enumerate(pixels):
        data[index] = round(linear_to_srgb(value) * 255.0)
    with path.open("wb") as file:
        file.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        file.write(data)


def luminance(pixels: list[float]) -> list[float]:
    return [0.2126 * pixels[index] + 0.7152 * pixels[index + 1]
            + 0.0722 * pixels[index + 2]
            for index in range(0, len(pixels), 3)]


def global_ssim(a: list[float], b: list[float]) -> float:
    if len(a) != len(b):
        return 0.0
    mean_a = sum(a) / len(a)
    mean_b = sum(b) / len(b)
    variance_a = sum((value - mean_a) ** 2 for value in a) / len(a)
    variance_b = sum((value - mean_b) ** 2 for value in b) / len(b)
    covariance = sum((x - mean_a) * (y - mean_b) for x, y in zip(a, b)) / len(a)
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    return ((2.0 * mean_a * mean_b + c1) * (2.0 * covariance + c2)
            / ((mean_a ** 2 + mean_b ** 2 + c1) * (variance_a + variance_b + c2)))


def image_metrics(width: int, height: int, pixels: list[float], local: bool) -> dict:
    if not all(math.isfinite(value) for value in pixels):
        raise RuntimeError("pipeline capture contains NaN or Inf")
    pixel_count = width * height
    clipped_low = sum(1 for value in pixels if value <= 0.0) / len(pixels)
    clipped_high = sum(1 for value in pixels if value >= 1.0) / len(pixels)
    neutral_error = 0.0
    for y in range(max(0, height // 64), max(1, height // 8 - height // 64)):
        for x in range(width):
            index = (y * width + x) * 3
            neutral_error = max(neutral_error,
                                abs(pixels[index] - pixels[index + 1]),
                                abs(pixels[index + 1] - pixels[index + 2]))
    uniform_range = 0.0
    y0, y1 = int(height * 0.90), int(height * 0.96)
    for patch in range(3):
        x0 = int(width * (patch / 3.0 + 0.08))
        x1 = int(width * ((patch + 1) / 3.0 - 0.08))
        for channel in range(3):
            values = [pixels[(y * width + x) * 3 + channel]
                      for y in range(y0, y1) for x in range(x0, x1)]
            uniform_range = max(uniform_range, max(values) - min(values))
    # A chart patch is influenced by neighboring bands in local mode; isolated
    # full-frame constants receive the strict gate below.
    threshold = 0.06 if local else 0.002
    if uniform_range > threshold:
        raise RuntimeError(
            f"constant-field variation {uniform_range} exceeds {threshold} ({'local' if local else 'global'})")
    maximum_adjacent_delta = 0.0
    for y in range(height):
        for x in range(width - 1):
            left = (y * width + x) * 3
            right = left + 3
            maximum_adjacent_delta = max(maximum_adjacent_delta,
                *(abs(pixels[left + channel] - pixels[right + channel]) for channel in range(3)))
    return {
        "minimum": min(pixels),
        "maximum": max(pixels),
        "clipped_low_fraction": clipped_low,
        "clipped_high_fraction": clipped_high,
        "maximum_neutral_axis_error": neutral_error,
        "constant_field_range": uniform_range,
        "maximum_horizontal_delta": maximum_adjacent_delta,
        "pixel_count": pixel_count,
    }


def run_pipeline_matrix(report_dir: Path, frame: int) -> dict:
    game = ROOT / "bin/game"
    if platform.system() == "Windows":
        game = ROOT / "bin/game.exe"
    if not game.is_file():
        raise RuntimeError("game binary is missing; run build.sh before pipeline validation")
    capture_dir = report_dir / "pipeline"
    preview_dir = report_dir / "previews"
    log_dir = report_dir / "logs"
    capture_dir.mkdir(parents=True, exist_ok=True)
    preview_dir.mkdir(parents=True, exist_ok=True)
    results: list[dict] = []
    images: dict[tuple[str, str, bool], tuple[int, int, list[float]]] = {}
    for output_mode in OUTPUTS:
        for method in METHODS:
            for local in (False, True):
                label = f"{output_mode}-{method}-{'local' if local else 'global'}"
                prefix = capture_dir / label
                environment = os.environ.copy()
                configure_moltenvk(environment)
                environment.update({
                    "GAME2_OUTPUT_MODE": "sdr",
                    "GAME2_TONEMAP_VALIDATION_CHART": "1",
                    "GAME2_TONEMAP_VALIDATION_OUTPUT_MODE": output_mode,
                    "GAME2_TONEMAP_VALIDATION_CAPTURE": str(prefix),
                    "GAME2_SCREENSHOT_FRAME": str(frame),
                    "GAME2_TONEMAP_MODE": method,
                    "GAME2_LOCAL_TONEMAP": "1" if local else "0",
                    "GAME2_RENDER_SCALE": "100",
                    "GAME2_HIDE_UI": "1",
                    "GAME2_BLOOM": "0",
                    "GAME2_TAA": "0",
                    "GAME2_FXAA": "0",
                    "GAME2_SSAO": "0",
                    "GAME2_DOF": "0",
                })
                run([str(game), "--no-live-link"], env=environment,
                    output=log_dir / f"{label}.log")
                first = Path(f"{prefix}.repeat0.tonemapped.pfm")
                repeat = Path(f"{prefix}.repeat1.tonemapped.pfm")
                width, height, pixels = read_pfm(first)
                repeat_width, repeat_height, repeat_pixels = read_pfm(repeat)
                if (width, height) != (repeat_width, repeat_height):
                    raise RuntimeError(f"{label}: repeat dimensions differ")
                repeat_error = max(abs(a - b) for a, b in zip(pixels, repeat_pixels))
                if repeat_error > 1e-6:
                    raise RuntimeError(f"{label}: temporal determinism error {repeat_error}")
                if output_mode == "sdr" and (min(pixels) < -0.002 or max(pixels) > 1.002):
                    raise RuntimeError(f"{label}: SDR result is outside [0,1]")
                metrics = image_metrics(width, height, pixels, local)
                metrics["maximum_repeat_error"] = repeat_error
                preview = preview_dir / f"{label}.ppm"
                write_preview(preview, width, height, pixels)
                images[(output_mode, method, local)] = (width, height, pixels)
                results.append({"name": label, "output_mode": output_mode,
                                "method": method, "local": local,
                                "preview": str(preview.relative_to(report_dir)), **metrics})
    for result in results:
        if not result["local"]:
            continue
        key = (result["output_mode"], result["method"])
        _, _, global_pixels = images[(key[0], key[1], False)]
        _, _, local_pixels = images[(key[0], key[1], True)]
        result["global_local_luminance_ssim"] = global_ssim(
            luminance(global_pixels), luminance(local_pixels))
    constant_tests: list[dict] = []
    for output_mode in OUTPUTS:
        for method in METHODS:
            label = f"{output_mode}-{method}-local-constant"
            prefix = capture_dir / label
            environment = os.environ.copy()
            configure_moltenvk(environment)
            environment.update({
                "GAME2_OUTPUT_MODE": "sdr",
                "GAME2_TONEMAP_VALIDATION_CHART": "constant",
                "GAME2_TONEMAP_VALIDATION_OUTPUT_MODE": output_mode,
                "GAME2_TONEMAP_VALIDATION_CAPTURE": str(prefix),
                "GAME2_SCREENSHOT_FRAME": str(frame),
                "GAME2_TONEMAP_MODE": method,
                "GAME2_LOCAL_TONEMAP": "1",
                "GAME2_RENDER_SCALE": "100",
                "GAME2_HIDE_UI": "1",
                "GAME2_BLOOM": "0", "GAME2_TAA": "0", "GAME2_FXAA": "0",
                "GAME2_SSAO": "0", "GAME2_DOF": "0",
            })
            run([str(game), "--no-live-link"], env=environment,
                output=log_dir / f"{label}.log")
            width, height, pixels = read_pfm(Path(f"{prefix}.repeat0.tonemapped.pfm"))
            _, _, repeated = read_pfm(Path(f"{prefix}.repeat1.tonemapped.pfm"))
            repeat_error = max(abs(a - b) for a, b in zip(pixels, repeated))
            interior_min = math.inf
            interior_max = -math.inf
            for y in range(height // 8, height * 7 // 8):
                for x in range(width // 8, width * 7 // 8):
                    index = (y * width + x) * 3
                    for value in pixels[index:index + 3]:
                        interior_min = min(interior_min, value)
                        interior_max = max(interior_max, value)
            uniform_range = interior_max - interior_min
            if repeat_error > 1e-6 or uniform_range > 0.003:
                raise RuntimeError(
                    f"{label}: repeat={repeat_error}, constant-field range={uniform_range}")
            constant_tests.append({"name": label, "maximum_repeat_error": repeat_error,
                                   "constant_field_range": uniform_range})
    report = {"suite": "tonemapping-full-pipeline-v1", "passed": True,
              "configuration_count": len(results), "configurations": results,
              "constant_field_tests": constant_tests}
    (report_dir / "pipeline_validation.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    write_html(report_dir, report)
    return report


def write_html(report_dir: Path, pipeline: dict) -> None:
    rows = []
    for result in pipeline["configurations"]:
        rows.append(
            "<tr><td>{}</td><td><img src=\"{}\" width=\"384\"></td>"
            "<td>min {:.5g}<br>max {:.5g}<br>clip low {:.3%}<br>clip high {:.3%}"
            "<br>neutral error {:.5g}<br>constant range {:.5g}{}</td></tr>".format(
                html.escape(result["name"]), html.escape(result["preview"]),
                result["minimum"], result["maximum"], result["clipped_low_fraction"],
                result["clipped_high_fraction"], result["maximum_neutral_axis_error"],
                result["constant_field_range"],
                ("<br>global/local SSIM {:.6f}".format(result["global_local_luminance_ssim"])
                 if "global_local_luminance_ssim" in result else "")))
    document = """<!doctype html><meta charset="utf-8"><title>Tonemapping validation</title>
<style>body{{font:14px system-ui;background:#17191d;color:#eee;margin:24px}}table{{border-collapse:collapse}}
td,th{{border:1px solid #555;padding:8px;vertical-align:top}}img{{image-rendering:auto}}</style>
<h1>Tonemapping validation review</h1><p>Numerical gates passed. Images are review artifacts, not goldens.</p>
<table><tr><th>Configuration</th><th>Procedural chart</th><th>Metrics</th></tr>{}</table>""".format(
        "\n".join(rows))
    (report_dir / "index.html").write_text(document, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=ROOT / "bin/validation/tonemapping")
    parser.add_argument("--skip-gpu", action="store_true")
    parser.add_argument("--skip-pipeline", action="store_true")
    parser.add_argument("--skip-ocio", action="store_true")
    parser.add_argument("--ocio-python", default=sys.executable)
    parser.add_argument("--rebuild-game", action="store_true")
    parser.add_argument("--capture-frame", type=int, default=8)
    args = parser.parse_args()
    report_dir = args.output_dir.resolve()
    binary_dir = report_dir / "bin"
    binary_dir.mkdir(parents=True, exist_ok=True)
    report_dir.mkdir(parents=True, exist_ok=True)

    os_name = {"Darwin": "Mac", "Linux": "Linux", "Windows": "Windows"}.get(platform.system())
    if not os_name:
        raise RuntimeError(f"unsupported platform {platform.system()}")
    run(["./compile_shaders.sh", os_name])
    compile_cpu_tests(binary_dir)
    cpu = run_cpu_suite(binary_dir, report_dir)
    gpu = None
    if not args.skip_gpu:
        compile_gpu_test(binary_dir)
        gpu = run_gpu_suite(binary_dir, report_dir)
    ocio = None
    if not args.skip_ocio:
        ocio = verify_ocio_regeneration(args.ocio_python, report_dir)
    pipeline = None
    if not args.skip_pipeline:
        if args.rebuild_game or not (ROOT / ("bin/game.exe" if os_name == "Windows" else "bin/game")).is_file():
            run(["./build.sh", os_name, "-norun"])
        pipeline = run_pipeline_matrix(report_dir, args.capture_frame)
    summary = {
        "suite": "formal-tonemapping-validation-v1",
        "passed": True,
        "cpu": cpu,
        "gpu": gpu,
        "ocio": ocio,
        "pipeline": pipeline,
    }
    (report_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Tonemapping validation passed; report: {report_dir / 'summary.json'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Tonemapping validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
