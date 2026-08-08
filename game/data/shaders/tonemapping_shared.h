#ifndef TONEMAPPING_SHARED_H
#define TONEMAPPING_SHARED_H

// Numeric ABI shared by runtime state, GLSL dispatch, LUT packing, and the
// standalone GPU conformance harness. Keep method indices stable.
#define TONEMAP_METHOD_GT7 0
#define TONEMAP_METHOD_AGX 1
#define TONEMAP_METHOD_ACES_2 2
#define TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL 3
#define TONEMAP_METHOD_COUNT 4

#define TONEMAP_LUT_RESOLUTION 64
#define TONEMAP_LUT_INPUT_MAX 64.0
#define TONEMAP_GT7_LAYER_OFFSET 0
#define TONEMAP_ACES2_LAYER_OFFSET 64
#define TONEMAP_AGX_LAYER_OFFSET 128
#define TONEMAP_LUT_LAYER_COUNT 192

#endif
