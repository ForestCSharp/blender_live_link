#!/usr/bin/env python3
"""Run deterministic CPU, GPU, and OCIO tonemapping validation."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CPU_TESTS = (
    "auto_adaptation_tests",
    "gt7_tonemapping_tests",
    "aces2_tonemapping_tests",
    "agx_tonemapping_tests",
    "pbr_neutral_tonemapping_tests",
    "display_encoding_tests",
    "output_selection_tests",
    "sky_aware_tonemapping_tests",
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=ROOT / "bin/validation/tonemapping")
    parser.add_argument("--skip-gpu", action="store_true")
    parser.add_argument("--skip-ocio", action="store_true")
    parser.add_argument("--ocio-python", default=sys.executable)
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
    summary = {
        "suite": "standalone-tonemapping-validation-v2",
        "passed": True,
        "cpu": cpu,
        "gpu": gpu,
        "ocio": ocio,
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
