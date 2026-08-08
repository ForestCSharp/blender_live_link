#!/usr/bin/env python3
"""Generate checked-in Blender 5.2 AgX SDR and HDR tone-mapping LUTs.

Requires Python 3, NumPy, and PyOpenColorIO 2.5.0. Source files are fetched
from the pinned Blender release and SHA-256 verified. The normal game build
and runtime do not depend on OpenColorIO or network access.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import urllib.request
import zlib

import numpy as np
import PyOpenColorIO as ocio


BLENDER_VERSION = "v5.2.0"
BLENDER_COMMIT = "fbe6228777e7d9afefcd61a413844e790ae75db7"
BLENDER_RAW_ROOT = (
    f"https://raw.githubusercontent.com/blender/blender/{BLENDER_VERSION}/"
    "release/datafiles/colormanagement"
)
OCIO_VERSION = "2.5.0"
AGX_LOOK = "AgX - Medium High Contrast"

SOURCES = {
    "config.ocio": "47a7d83e79c1d21f49ba6c505efe311da723471688c614b5c366e1da7eb8ea3a",
    "luts/AgX_Base_sRGB.cube": "e707a36f3e90ee79bc342332febf91334c02ce3974cac700ece00ca9d4507491",
    "luts/AgX_Rec2100-HLG_p3_lim.cube": "4422eb9a8d3ecc16836d241287b171758e2f2db202934d9ff51865543dd360c0",
}

# OCIO resolves all referenced looks while constructing a legacy viewing
# pipeline. These pinned-tag support files are cached so processor creation does
# not depend on a partial Blender installation; only the AgX sources above feed
# the generated payloads and carry explicit content hashes in the manifest.
SUPPORT_FILES = (
    "luts/AgX_Base_P3.cube",
    "luts/AgX_Base_Rec2020.cube",
    "luts/AgX_False_Color.spi1d",
    "luts/luminance_compensation_bt2020.cube",
    "luts/pbrNeutral.cube",
    "luts/xyz_E_to_D65.spimtx",
    "filmic/filmic_desat_33.cube",
    "filmic/filmic_to_0-35_1-30.spi1d",
    "filmic/filmic_to_0-48_1-09.spi1d",
    "filmic/filmic_to_0-60_1-04.spi1d",
    "filmic/filmic_to_0-70_1-03.spi1d",
    "filmic/filmic_to_0-85_1-011.spi1d",
    "filmic/filmic_to_0.99_1-0075.spi1d",
    "filmic/filmic_to_1.20_1-00.spi1d",
)

LUT_MAGIC = b"G2AGXLUT"
LUT_VERSION = 1
LUT_RESOLUTION = 64
LUT_INPUT_MAX = 64.0
LUT_SHAPER_ACESCCT = 1
LUT_PIXEL_FORMAT_RGBA16F = 1
LUT_HEADER = struct.Struct("<8s8I")

SDR_MIDDLE_GRAY_TARGET = 0.214519
HDR_MIDDLE_GRAY_TARGET = 0.203

# These constants are checked against the deterministic root solve. Update them
# only when the pinned source transform or integration policy changes.
AGX_SDR_INTEGRATION_SCALE = 1.1601751
AGX_HDR_INTEGRATION_SCALE = 5.1099494

TARGETS = (
    {
        "name": "sdr",
        "id": 0,
        "display": "sRGB",
        "view": "AgX",
        "peak_nits": 100.0,
        "middle_gray_target": SDR_MIDDLE_GRAY_TARGET,
        "runtime_integration_scale": AGX_SDR_INTEGRATION_SCALE,
    },
    {
        "name": "hdr1000",
        "id": 1,
        "display": "Rec.2100-PQ",
        "view": "AgX - HDR 1000 nits",
        "peak_nits": 1000.0,
        "middle_gray_target": HDR_MIDDLE_GRAY_TARGET,
        "runtime_integration_scale": AGX_HDR_INTEGRATION_SCALE,
    },
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch_sources(source_directory: Path) -> dict[str, str]:
    resolved = {}
    for relative_path, expected_hash in SOURCES.items():
        destination = source_directory / relative_path
        if destination.exists():
            data = destination.read_bytes()
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            url = f"{BLENDER_RAW_ROOT}/{relative_path}"
            print(f"fetching {url}")
            with urllib.request.urlopen(url) as response:
                data = response.read()
            destination.write_bytes(data)
        actual_hash = sha256(data)
        if actual_hash != expected_hash:
            raise RuntimeError(
                f"{relative_path}: SHA-256 {actual_hash} does not match {expected_hash}")
        resolved[relative_path] = actual_hash
    for relative_path in SUPPORT_FILES:
        destination = source_directory / relative_path
        if destination.exists():
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        url = f"{BLENDER_RAW_ROOT}/{relative_path}"
        print(f"fetching support file {url}")
        with urllib.request.urlopen(url) as response:
            destination.write_bytes(response.read())
    return resolved


def acescct_from_linear(value: np.ndarray | float) -> np.ndarray | float:
    value = np.asarray(value)
    result = 10.5402377416545 * value + 0.0729055341958355
    logarithmic = value > 0.0078125
    result = np.asarray(result)
    result[logarithmic] = (np.log2(value[logarithmic]) + 9.72) / 17.52
    return result


def linear_from_acescct(value: np.ndarray) -> np.ndarray:
    return np.where(
        value <= 0.155251141552511,
        (value - 0.0729055341958355) / 10.5402377416545,
        np.exp2(value * 17.52 - 9.72),
    )


def lut_axis() -> np.ndarray:
    low = float(acescct_from_linear(0.0))
    high = float(acescct_from_linear(LUT_INPUT_MAX))
    encoded = np.linspace(low, high, LUT_RESOLUTION, dtype=np.float64)
    return linear_from_acescct(encoded).astype(np.float32)


def srgb_to_linear(value: np.ndarray) -> np.ndarray:
    return np.where(
        value <= 0.04045,
        value / 12.92,
        np.power((value + 0.055) / 1.055, 2.4),
    )


def pq_to_nits(value: np.ndarray) -> np.ndarray:
    m1 = 0.1593017578125
    m2 = 78.84375
    c1 = 0.8359375
    c2 = 18.8515625
    c3 = 18.6875
    power = np.power(np.maximum(value, 0.0), 1.0 / m2)
    normalized = np.power(
        np.maximum(power - c1, 0.0) / np.maximum(c2 - c3 * power, 1e-12),
        1.0 / m1,
    )
    return normalized * 10000.0


def rec2020_to_linear_srgb(value: np.ndarray) -> np.ndarray:
    matrix = np.array([
        [1.6604910, -0.5876411, -0.0728499],
        [-0.1245505, 1.1328999, -0.0083494],
        [-0.0181508, -0.1005789, 1.1187297],
    ], dtype=np.float32)
    return value @ matrix.T


def linear_srgb_to_rec2020(value: np.ndarray) -> np.ndarray:
    matrix = np.array([
        [0.6274039, 0.3292830, 0.0433131],
        [0.0690973, 0.9195404, 0.0113623],
        [0.0163914, 0.0880133, 0.8955953],
    ], dtype=np.float32)
    return value @ matrix.T


def make_processor(config: ocio.Config, target: dict):
    transform = ocio.DisplayViewTransform()
    transform.setSrc("Linear Rec.709")
    transform.setDisplay(target["display"])
    transform.setView(target["view"])
    pipeline = ocio.LegacyViewingPipeline()
    pipeline.setDisplayViewTransform(transform)
    pipeline.setLooksOverride(AGX_LOOK)
    pipeline.setLooksOverrideEnabled(True)
    return pipeline.getProcessor(config).getDefaultCPUProcessor()


def apply_processor(processor, pixels: np.ndarray) -> None:
    image = ocio.PackedImageDesc(pixels, pixels.shape[1], pixels.shape[0], 3)
    processor.apply(image)


def apply_reference(target: dict, input_pixels: np.ndarray, processor) -> np.ndarray:
    pixels = np.array(input_pixels, dtype=np.float32, order="C", copy=True).reshape(1, -1, 3)
    apply_processor(processor, pixels)
    pixels = pixels.reshape(-1, 3)
    if target["name"] == "sdr":
        return np.clip(srgb_to_linear(pixels), 0.0, 1.0).astype(np.float32)
    rec2020 = np.clip(pq_to_nits(pixels) / target["peak_nits"], 0.0, 1.0)
    return rec2020_to_linear_srgb(rec2020).astype(np.float32)


def solve_integration_scale(target: dict, processor) -> float:
    def mapped_gray(scale: float) -> float:
        source = np.array([[0.18 * scale] * 3], dtype=np.float32)
        return float(np.mean(apply_reference(target, source, processor)))

    low = 0.0
    high = 1.0
    while mapped_gray(high) < target["middle_gray_target"]:
        high *= 2.0
    for _ in range(48):
        middle = (low + high) * 0.5
        if mapped_gray(middle) < target["middle_gray_target"]:
            low = middle
        else:
            high = middle
    return (low + high) * 0.5


def generate_payload(target: dict, processor) -> tuple[bytes, np.ndarray]:
    axis = lut_axis()
    r = np.tile(axis, LUT_RESOLUTION * LUT_RESOLUTION)
    g = np.tile(np.repeat(axis, LUT_RESOLUTION), LUT_RESOLUTION)
    b = np.repeat(axis, LUT_RESOLUTION * LUT_RESOLUTION)
    input_pixels = np.stack((r, g, b), axis=1)
    output = apply_reference(target, input_pixels, processor)
    rgba = np.ones((output.shape[0], 4), dtype=np.float32)
    rgba[:, :3] = output
    return rgba.astype("<f2").tobytes(order="C"), output


def normalized_acescct(value: np.ndarray) -> np.ndarray:
    value = np.clip(value, 0.0, LUT_INPUT_MAX)
    low = float(acescct_from_linear(0.0))
    high = float(acescct_from_linear(LUT_INPUT_MAX))
    return (acescct_from_linear(value) - low) / (high - low)


def sample_lut(output: np.ndarray, inputs: np.ndarray) -> np.ndarray:
    positions = normalized_acescct(inputs) * (LUT_RESOLUTION - 1)
    lower = np.floor(positions).astype(np.int32)
    upper = np.minimum(lower + 1, LUT_RESOLUTION - 1)
    fraction = positions - lower
    cube = output.reshape(LUT_RESOLUTION, LUT_RESOLUTION, LUT_RESOLUTION, 3)
    result = np.empty_like(inputs, dtype=np.float32)
    for sample_index in range(inputs.shape[0]):
        r0, g0, b0 = lower[sample_index]
        r1, g1, b1 = upper[sample_index]
        fr, fg, fb = fraction[sample_index]
        c000, c100 = cube[b0, g0, r0], cube[b0, g0, r1]
        c010, c110 = cube[b0, g1, r0], cube[b0, g1, r1]
        c001, c101 = cube[b1, g0, r0], cube[b1, g0, r1]
        c011, c111 = cube[b1, g1, r0], cube[b1, g1, r1]
        c00 = c000 * (1.0 - fr) + c100 * fr
        c10 = c010 * (1.0 - fr) + c110 * fr
        c01 = c001 * (1.0 - fr) + c101 * fr
        c11 = c011 * (1.0 - fr) + c111 * fr
        result[sample_index] = (
            (c00 * (1.0 - fg) + c10 * fg) * (1.0 - fb)
            + (c01 * (1.0 - fg) + c11 * fg) * fb
        )
    return result


def solve_sampled_integration_scale(target: dict, quantized_output: np.ndarray) -> float:
    def mapped_gray(scale: float) -> float:
        source = np.array([[0.18 * scale] * 3], dtype=np.float32)
        return float(np.mean(sample_lut(quantized_output, source)))

    low = 0.0
    high = 1.0
    while mapped_gray(high) < target["middle_gray_target"]:
        high *= 2.0
    for _ in range(48):
        middle = (low + high) * 0.5
        if mapped_gray(middle) < target["middle_gray_target"]:
            low = middle
        else:
            high = middle
    return (low + high) * 0.5


def verify_lut(target: dict, quantized_output: np.ndarray, processor) -> dict:
    rng = np.random.default_rng(0xA65_520)
    random_inputs = LUT_INPUT_MAX * np.power(
        rng.random((20000, 3), dtype=np.float32), 4.0)
    directed = np.array([
        [0.0, 0.0, 0.0], [0.18, 0.18, 0.18], [1.0, 1.0, 1.0],
        [64.0, 64.0, 64.0], [4.0, 2.0, 0.5], [12.0, 0.25, 2.0],
        [64.0, 0.0, 0.0], [0.0, 64.0, 0.0], [0.0, 0.0, 64.0],
    ], dtype=np.float32)
    inputs = np.concatenate((directed, random_inputs), axis=0)
    analytic = apply_reference(target, inputs, processor)
    sampled = sample_lut(quantized_output, inputs)
    errors = np.abs(sampled - analytic).reshape(-1)
    mean_error = float(np.mean(errors))
    p99_error = float(np.percentile(errors, 99.0))
    max_error = float(np.max(errors))
    if not np.all(np.isfinite(sampled)):
        raise RuntimeError(f"{target['name']}: LUT produced non-finite values")
    if mean_error >= 0.005 or p99_error >= 0.02:
        raise RuntimeError(
            f"{target['name']}: LUT error exceeds limits: mean={mean_error}, p99={p99_error}")

    gray = sample_lut(
        quantized_output,
        np.array([[0.18 * target["integration_scale"]] * 3], dtype=np.float32),
    )[0]
    if abs(float(np.mean(gray)) - target["middle_gray_target"]) >= 0.001:
        raise RuntimeError(
            f"{target['name']}: middle gray {gray} does not match {target['middle_gray_target']}")

    if target["name"] == "sdr":
        if np.min(sampled) < -0.001 or np.max(sampled) > 1.001:
            raise RuntimeError("sdr: LUT output is outside linear Rec.709 bounds")
    else:
        rec2020 = linear_srgb_to_rec2020(sampled)
        if np.min(rec2020) < -0.002 or np.max(rec2020) > 1.002:
            raise RuntimeError("hdr1000: LUT output is outside normalized Rec.2020 bounds")

    reference_inputs = np.array([
        [0.0, 0.0, 0.0], [0.18, 0.18, 0.18], [1.0, 1.0, 1.0],
        [4.0, 2.0, 0.5], [12.0, 0.25, 2.0], [64.0, 64.0, 64.0],
    ], dtype=np.float32)
    reference_outputs = apply_reference(target, reference_inputs, processor)
    return {
        "mean_absolute_rgb_error": mean_error,
        "p99_absolute_rgb_error": p99_error,
        "max_absolute_rgb_error": max_error,
        "middle_gray_output": [float(channel) for channel in gray],
        "reference_vectors": [
            {"input": source.tolist(), "output": output.tolist()}
            for source, output in zip(reference_inputs, reference_outputs)
        ],
    }


def write_target(
        output_directory: Path, target: dict, processor,
        analytic_integration_scale: float) -> dict:
    payload, _ = generate_payload(target, processor)
    quantized = np.frombuffer(payload, dtype="<f2").reshape(-1, 4)[:, :3].astype(np.float32)
    sampled_scale = solve_sampled_integration_scale(target, quantized)
    runtime_scale = target["runtime_integration_scale"]
    if abs(sampled_scale - runtime_scale) >= 1e-5:
        raise RuntimeError(
            f"{target['name']}: sampled root-solved integration scale {sampled_scale} "
            f"does not match runtime constant {runtime_scale}")
    resolved_target = dict(target)
    resolved_target["integration_scale"] = runtime_scale
    resolved_target["root_solved_integration_scale"] = sampled_scale
    resolved_target["analytic_root_solved_integration_scale"] = analytic_integration_scale
    verification = verify_lut(resolved_target, quantized, processor)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = LUT_HEADER.pack(
        LUT_MAGIC, LUT_VERSION, LUT_RESOLUTION, target["id"],
        LUT_SHAPER_ACESCCT, LUT_PIXEL_FORMAT_RGBA16F, len(payload), crc, 0)
    path = output_directory / f"agx_{target['name']}.lutbin"
    path.write_bytes(header + payload)
    return {
        "name": target["name"],
        "file": path.name,
        "target_id": target["id"],
        "display": target["display"],
        "view": target["view"],
        "look": AGX_LOOK,
        "peak_nits": target["peak_nits"],
        "integration_scale": resolved_target["integration_scale"],
        "root_solved_integration_scale": resolved_target["root_solved_integration_scale"],
        "analytic_root_solved_integration_scale":
            resolved_target["analytic_root_solved_integration_scale"],
        "middle_gray_target": target["middle_gray_target"],
        "payload_bytes": len(payload),
        "crc32": f"{crc:08x}",
        **verification,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir", type=Path,
        default=Path(__file__).resolve().parents[1] / "data" / "tonemapping")
    parser.add_argument(
        "--source-cache", type=Path,
        default=Path(tempfile.gettempdir()) / "game2-agx-blender-v5.2.0")
    parser.add_argument(
        "--print-scales", action="store_true",
        help="print root-solved scales without requiring constants to match")
    args = parser.parse_args()
    if ocio.__version__ != OCIO_VERSION:
        raise RuntimeError(f"PyOpenColorIO {OCIO_VERSION} is required; found {ocio.__version__}")
    source_hashes = fetch_sources(args.source_cache)
    config = ocio.Config.CreateFromFile(str(args.source_cache / "config.ocio"))

    processors = {}
    analytic_scales = {}
    for target in TARGETS:
        processor = make_processor(config, target)
        processors[target["name"]] = processor
        root_solved_scale = solve_integration_scale(target, processor)
        analytic_scales[target["name"]] = root_solved_scale
        print(f"{target['name']} root-solved integration scale: {root_solved_scale:.9f}")
    if args.print_scales:
        for target in TARGETS:
            payload, _ = generate_payload(target, processors[target["name"]])
            quantized = np.frombuffer(payload, dtype="<f2").reshape(-1, 4)[:, :3].astype(np.float32)
            sampled_scale = solve_sampled_integration_scale(target, quantized)
            print(f"{target['name']} sampled integration scale: {sampled_scale:.9f}")
        return 0

    args.output_dir.mkdir(parents=True, exist_ok=True)
    targets = [
        write_target(
            args.output_dir, target, processors[target["name"]],
            analytic_scales[target["name"]])
        for target in TARGETS
    ]
    manifest = {
        "format": "game2-agx-lut-manifest-v1",
        "blender_version": BLENDER_VERSION,
        "blender_commit": BLENDER_COMMIT,
        "opencolorio_version": OCIO_VERSION,
        "source_sha256": source_hashes,
        "resolution": LUT_RESOLUTION,
        "input_max": LUT_INPUT_MAX,
        "shaper": "normalized ACEScct",
        "pixel_format": "RGBA16F little-endian",
        "targets": targets,
    }
    manifest_path = args.output_dir / "agx_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for target in targets:
        print(
            f"{target['name']}: crc={target['crc32']} "
            f"mean={target['mean_absolute_rgb_error']:.7f} "
            f"p99={target['p99_absolute_rgb_error']:.7f} "
            f"max={target['max_absolute_rgb_error']:.7f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
