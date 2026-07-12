#version 450

// Drawn at the far plane (z = 0 under reverse-Z) with depth test on, so sky
// fragments only fill pixels no geometry wrote
#define FULLSCREEN_MAX_DEPTH
#include "fullscreen_vs.h"
