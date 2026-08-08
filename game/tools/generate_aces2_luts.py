#!/usr/bin/env python3
"""Generate the checked-in ACES 2.0 tone-mapping LUTs.

Requires Python 3, NumPy, and PyOpenColorIO 2.5.0. The normal game build and
runtime do not depend on OpenColorIO.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

import numpy as np
import PyOpenColorIO as ocio


ACES_RELEASE = "v2.0.0+2025.04.04"
ACES_RELEASE_COMMIT = "35e1e6ac2c26ec75433547d5d0a3a881f39bd9f5"
ACES_CORE_COMMIT = "2d7af39344725aaa8ac3bf1746693c9a1d6c4792"
ACES_OUTPUT_COMMIT = "aab74723f76728c37345ed01e51ebb24fb1f2f1f"
OCIO_VERSION = "2.5.0"
OCIO_CONFIG = "cg-config-v4.0.0_aces-v2.0_ocio-v2.5"

LUT_MAGIC = b"G2A2LUT\0"
LUT_VERSION = 1
LUT_RESOLUTION = 64
LUT_INPUT_MAX = 64.0
LUT_SHAPER_ACESCCT = 1
LUT_PIXEL_FORMAT_RGBA16F = 1
LUT_HEADER = struct.Struct("<8s8I")

ACES2_SDR_INTEGRATION_SCALE = 2.0548065
ACES2_HDR_INTEGRATION_SCALE = 10.9398375
SDR_MIDDLE_GRAY_TARGET = 0.214519
HDR_MIDDLE_GRAY_TARGET = 0.203

REC709_D65 = (0.6400, 0.3300, 0.3000, 0.6000, 0.1500, 0.0600, 0.3127, 0.3290)
REC2020_D65 = (0.7080, 0.2920, 0.1700, 0.7970, 0.1310, 0.0460, 0.3127, 0.3290)

TARGETS = (
    {"name": "sdr", "id": 0, "peak_nits": 100.0, "primaries": REC709_D65,
     "middle_gray_target": SDR_MIDDLE_GRAY_TARGET,
     "runtime_integration_scale": ACES2_SDR_INTEGRATION_SCALE},
    {"name": "edr", "id": 1, "peak_nits": 1000.0, "primaries": REC709_D65,
     "middle_gray_target": HDR_MIDDLE_GRAY_TARGET,
     "runtime_integration_scale": ACES2_HDR_INTEGRATION_SCALE},
    {"name": "hdr10", "id": 2, "peak_nits": 1000.0, "primaries": REC2020_D65,
     "middle_gray_target": HDR_MIDDLE_GRAY_TARGET,
     "runtime_integration_scale": ACES2_HDR_INTEGRATION_SCALE},
)


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


def make_processor(target: dict):
    config = ocio.Config.CreateFromBuiltinConfig(OCIO_CONFIG)
    to_aces = config.getProcessor(
        "Linear Rec.709 (sRGB)", "ACES2065-1").getDefaultCPUProcessor()
    transform = ocio.FixedFunctionTransform(
        style=ocio.FIXED_FUNCTION_ACES_OUTPUT_TRANSFORM_20,
        params=[target["peak_nits"], *target["primaries"]],
    )
    aces_output = ocio.Config.CreateRaw().getProcessor(transform).getDefaultCPUProcessor()
    to_srgb = None
    if target["name"] == "hdr10":
        to_srgb = config.getProcessor(
            "Linear Rec.2020", "Linear Rec.709 (sRGB)").getDefaultCPUProcessor()
    return to_aces, aces_output, to_srgb


def apply_processor(processor, pixels: np.ndarray) -> None:
    image = ocio.PackedImageDesc(pixels, pixels.shape[1], pixels.shape[0], 3)
    processor.apply(image)


def apply_reference(target: dict, input_pixels: np.ndarray, processors=None) -> np.ndarray:
    pixels = np.array(input_pixels, dtype=np.float32, order="C", copy=True).reshape(1, -1, 3)
    to_aces, aces_output, to_srgb = processors or make_processor(target)
    apply_processor(to_aces, pixels)
    apply_processor(aces_output, pixels)
    peak_units = target["peak_nits"] / 100.0
    np.clip(pixels, 0.0, peak_units, out=pixels)
    pixels /= peak_units
    if to_srgb is not None:
        apply_processor(to_srgb, pixels)
    return pixels.reshape(-1, 3).copy()


def solve_integration_scale(target: dict) -> float:
    """Solve the analytic ACES transform for the requested EV-0 middle gray."""
    processors = make_processor(target)

    def mapped_gray(scale: float) -> float:
        source = np.array([[0.18 * scale] * 3], dtype=np.float32)
        return float(np.mean(apply_reference(target, source, processors)))

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


def generate_payload(target: dict) -> tuple[bytes, np.ndarray]:
    axis = lut_axis()
    r = np.tile(axis, LUT_RESOLUTION * LUT_RESOLUTION)
    g = np.tile(np.repeat(axis, LUT_RESOLUTION), LUT_RESOLUTION)
    b = np.repeat(axis, LUT_RESOLUTION * LUT_RESOLUTION)
    input_pixels = np.stack((r, g, b), axis=1)
    output = apply_reference(target, input_pixels)
    rgba = np.ones((output.shape[0], 4), dtype=np.float32)
    rgba[:, :3] = output
    payload = rgba.astype("<f2").tobytes(order="C")
    return payload, output


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
        c000 = cube[b0, g0, r0]
        c100 = cube[b0, g0, r1]
        c010 = cube[b0, g1, r0]
        c110 = cube[b0, g1, r1]
        c001 = cube[b1, g0, r0]
        c101 = cube[b1, g0, r1]
        c011 = cube[b1, g1, r0]
        c111 = cube[b1, g1, r1]
        c00 = c000 * (1.0 - fr) + c100 * fr
        c10 = c010 * (1.0 - fr) + c110 * fr
        c01 = c001 * (1.0 - fr) + c101 * fr
        c11 = c011 * (1.0 - fr) + c111 * fr
        c0 = c00 * (1.0 - fg) + c10 * fg
        c1 = c01 * (1.0 - fg) + c11 * fg
        result[sample_index] = c0 * (1.0 - fb) + c1 * fb
    return result


def verify_lut(target: dict, quantized_output: np.ndarray) -> dict:
    rng = np.random.default_rng(0xACE520)
    random_inputs = LUT_INPUT_MAX * np.power(
        rng.random((20000, 3), dtype=np.float32), 4.0)
    directed = np.array([
        [0.0, 0.0, 0.0], [0.18, 0.18, 0.18], [1.0, 1.0, 1.0],
        [64.0, 64.0, 64.0], [4.0, 2.0, 0.5], [12.0, 0.25, 2.0],
        [64.0, 0.0, 0.0], [0.0, 64.0, 0.0], [0.0, 0.0, 64.0],
    ], dtype=np.float32)
    inputs = np.concatenate((directed, random_inputs), axis=0)
    analytic = apply_reference(target, inputs)
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

    scale = target["integration_scale"]
    gray_input = np.array([[0.18 * scale] * 3], dtype=np.float32)
    gray = sample_lut(quantized_output, gray_input)[0]
    expected_gray = target["middle_gray_target"]
    if abs(float(np.mean(gray)) - expected_gray) >= 0.001:
        raise RuntimeError(
            f"{target['name']}: middle gray {gray} does not match {expected_gray}")

    reference_inputs = np.array([
        [0.0, 0.0, 0.0], [0.18, 0.18, 0.18], [1.0, 1.0, 1.0],
        [4.0, 2.0, 0.5], [12.0, 0.25, 2.0], [64.0, 64.0, 64.0],
    ], dtype=np.float32)
    reference_outputs = apply_reference(target, reference_inputs)
    return {
        "mean_absolute_rgb_error": mean_error,
        "p99_absolute_rgb_error": p99_error,
        "max_absolute_rgb_error": max_error,
        "middle_gray_output": [float(channel) for channel in gray],
        "reference_vectors": [
            {"input": [float(v) for v in source], "output": [float(v) for v in output]}
            for source, output in zip(reference_inputs, reference_outputs)
        ],
    }


def write_target(output_directory: Path, target: dict) -> dict:
    payload, _ = generate_payload(target)
    quantized_output = np.frombuffer(payload, dtype="<f2").reshape(-1, 4)[:, :3].astype(np.float32)
    verification = verify_lut(target, quantized_output)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = LUT_HEADER.pack(
        LUT_MAGIC, LUT_VERSION, LUT_RESOLUTION, target["id"],
        LUT_SHAPER_ACESCCT, LUT_PIXEL_FORMAT_RGBA16F, len(payload), crc, 0)
    path = output_directory / f"aces2_{target['name']}.lutbin"
    path.write_bytes(header + payload)
    return {
        "name": target["name"],
        "file": path.name,
        "target_id": target["id"],
        "peak_nits": target["peak_nits"],
        "limiting_primaries": list(target["primaries"]),
        "integration_scale": target["integration_scale"],
        "root_solved_integration_scale": target["root_solved_integration_scale"],
        "middle_gray_target": target["middle_gray_target"],
        "payload_bytes": len(payload),
        "crc32": f"{crc:08x}",
        **verification,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "data" / "tonemapping",
    )
    args = parser.parse_args()
    if ocio.__version__ != OCIO_VERSION:
        raise RuntimeError(f"PyOpenColorIO {OCIO_VERSION} is required; found {ocio.__version__}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    resolved_targets = []
    for target in TARGETS:
        resolved_target = dict(target)
        root_solved_scale = solve_integration_scale(target)
        runtime_scale = target["runtime_integration_scale"]
        if abs(root_solved_scale - runtime_scale) >= 1e-5:
            raise RuntimeError(
                f"{target['name']}: root-solved integration scale {root_solved_scale} "
                f"does not match runtime constant {runtime_scale}")
        resolved_target["integration_scale"] = runtime_scale
        resolved_target["root_solved_integration_scale"] = root_solved_scale
        resolved_targets.append(resolved_target)
    targets = [write_target(args.output_dir, target) for target in resolved_targets]
    manifest = {
        "format": "game2-aces2-lut-manifest-v1",
        "aces_release": ACES_RELEASE,
        "aces_release_commit": ACES_RELEASE_COMMIT,
        "aces_core_commit": ACES_CORE_COMMIT,
        "aces_output_commit": ACES_OUTPUT_COMMIT,
        "opencolorio_version": OCIO_VERSION,
        "opencolorio_config": OCIO_CONFIG,
        "resolution": LUT_RESOLUTION,
        "input_max": LUT_INPUT_MAX,
        "shaper": "normalized ACEScct",
        "pixel_format": "RGBA16F little-endian",
        "targets": targets,
    }
    manifest_path = args.output_dir / "aces2_manifest.json"
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
