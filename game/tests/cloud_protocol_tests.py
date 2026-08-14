#!/usr/bin/env python3
"""FlatBuffers round-trip coverage for the procedural cloud protocol."""

import math
import pathlib
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from compiled_schemas.python import flatbuffers
from compiled_schemas.python.Blender.LiveLink import CloudLayer
from compiled_schemas.python.Blender.LiveLink import CloudLayerProfile
from compiled_schemas.python.Blender.LiveLink import GameplayComponentCloudSystem
from compiled_schemas.python.Blender.LiveLink import Vec2


def build_layer(builder, profile, index):
    values = {
        "enabled": index != 3,
        "seed": (1 << 32) - 1 if index == 3 else index * 17,
        "base": 0.0 if index == 0 else 1000.0 * index,
        "thickness": 1.0 if index == 0 else 1500.0 * (index + 1),
        "coverage": index / 3.0,
        "density": 0.0 if index == 0 else 2.0,
        "shape": 100.0 + index * 5000.0,
        "detail": 10.0 + index * 500.0,
        "erosion": index / 3.0,
        "anvil": index / 3.0,
        "wind": 0.0 if index == 0 else 4.0,
        "forward": 0.0 if index == 0 else 0.95,
        "backward": -0.95 if index == 0 else 0.0,
        "blend": index / 3.0,
        "ambient": index / 3.0,
        "multi": index / 3.0,
    }
    CloudLayer.Start(builder)
    CloudLayer.AddEnabled(builder, values["enabled"])
    CloudLayer.AddProfile(builder, profile)
    CloudLayer.AddSeedOffset(builder, values["seed"])
    CloudLayer.AddBaseAltitudeM(builder, values["base"])
    CloudLayer.AddThicknessM(builder, values["thickness"])
    CloudLayer.AddCoverage(builder, values["coverage"])
    CloudLayer.AddDensity(builder, values["density"])
    CloudLayer.AddShapeScaleM(builder, values["shape"])
    CloudLayer.AddDetailScaleM(builder, values["detail"])
    CloudLayer.AddErosion(builder, values["erosion"])
    CloudLayer.AddAnvilBias(builder, values["anvil"])
    CloudLayer.AddWindMultiplier(builder, values["wind"])
    CloudLayer.AddPhaseForward(builder, values["forward"])
    CloudLayer.AddPhaseBackward(builder, values["backward"])
    CloudLayer.AddPhaseBlend(builder, values["blend"])
    CloudLayer.AddAmbientScale(builder, values["ambient"])
    CloudLayer.AddMultiScatteringStrength(builder, values["multi"])
    return CloudLayer.End(builder), values


class CloudProtocolTests(unittest.TestCase):
    def test_default_shadow_extent_is_eight_kilometers(self):
        builder = flatbuffers.Builder(64)
        GameplayComponentCloudSystem.Start(builder)
        root = GameplayComponentCloudSystem.End(builder)
        builder.Finish(root)

        decoded = GameplayComponentCloudSystem.GameplayComponentCloudSystem.GetRootAs(
            builder.Output(), 0)
        self.assertEqual(decoded.ShadowExtentM(), 8000.0)

    def test_all_profiles_and_boundary_values_round_trip(self):
        builder = flatbuffers.Builder(2048)
        profiles = [
            CloudLayerProfile.CloudLayerProfile.Stratus,
            CloudLayerProfile.CloudLayerProfile.Cumulus,
            CloudLayerProfile.CloudLayerProfile.Cumulonimbus,
            CloudLayerProfile.CloudLayerProfile.Cirrus,
        ]
        built = [build_layer(builder, profile, index)
                 for index, profile in enumerate(profiles)]
        layer_offsets = [item[0] for item in built]
        GameplayComponentCloudSystem.StartLayersVector(builder, len(layer_offsets))
        for offset in reversed(layer_offsets):
            builder.PrependUOffsetTRelative(offset)
        layers = builder.EndVector()

        GameplayComponentCloudSystem.Start(builder)
        GameplayComponentCloudSystem.AddEnabled(builder, True)
        GameplayComponentCloudSystem.AddSeed(builder, (1 << 32) - 1)
        GameplayComponentCloudSystem.AddWeatherWorldScaleM(builder, 500000.0)
        wind = Vec2.CreateVec2(builder, -1.0, 1.0)
        GameplayComponentCloudSystem.AddWindDirection(builder, wind)
        GameplayComponentCloudSystem.AddWindSpeedMS(builder, 200.0)
        GameplayComponentCloudSystem.AddShadowEnabled(builder, True)
        GameplayComponentCloudSystem.AddShadowExtentM(builder, 200000.0)
        GameplayComponentCloudSystem.AddLayers(builder, layers)
        root = GameplayComponentCloudSystem.End(builder)
        builder.Finish(root)

        decoded = GameplayComponentCloudSystem.GameplayComponentCloudSystem.GetRootAs(
            builder.Output(), 0)
        self.assertTrue(decoded.Enabled())
        self.assertEqual(decoded.Seed(), (1 << 32) - 1)
        self.assertEqual(decoded.LayersLength(), 4)
        self.assertAlmostEqual(decoded.WindDirection().X(), -1.0)
        self.assertAlmostEqual(decoded.WindDirection().Y(), 1.0)
        for index, profile in enumerate(profiles):
            layer = decoded.Layers(index)
            expected = built[index][1]
            self.assertEqual(layer.Profile(), profile)
            self.assertEqual(layer.Enabled(), expected["enabled"])
            self.assertEqual(layer.SeedOffset(), expected["seed"])
            self.assertTrue(math.isclose(layer.Coverage(), expected["coverage"], abs_tol=1e-6))
            self.assertTrue(math.isclose(layer.PhaseBackward(), expected["backward"], abs_tol=1e-6))
            self.assertTrue(math.isclose(layer.MultiScatteringStrength(), expected["multi"], abs_tol=1e-6))


if __name__ == "__main__":
    unittest.main()
