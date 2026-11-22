#ifndef AUTO_ADAPTATION_H
#define AUTO_ADAPTATION_H

#define AUTO_ADAPTATION_STATE_EXPOSURE_WHITE 0
#define AUTO_ADAPTATION_STATE_CURRENT_LOG_LMS 1
#define AUTO_ADAPTATION_STATE_TARGET_LOG_LMS 2
#define AUTO_ADAPTATION_STATE_WB_COLUMN_0 3
#define AUTO_ADAPTATION_STATE_WB_COLUMN_1 4
#define AUTO_ADAPTATION_STATE_WB_COLUMN_2 5
#define AUTO_ADAPTATION_STATE_STATUS 6
#define AUTO_ADAPTATION_STATE_SOLAR_DIAGNOSTICS 7
#define AUTO_ADAPTATION_STATE_VEC4_COUNT 8

struct HistogramBin
{
	uint count;
	uint sum_x;
	uint sum_y;
	uint padding;
};

const uint AUTO_ADAPTATION_HISTOGRAM_BIN_COUNT = 256u;
const uvec2 AUTO_ADAPTATION_METER_SIZE = uvec2(256, 144);
const float AUTO_ADAPTATION_MIDDLE_GRAY = 0.18;
const float AUTO_ADAPTATION_MIN_EV = -16.0;
const float AUTO_ADAPTATION_MAX_EV = 16.0;
const float AUTO_ADAPTATION_FIXED_SCALE = 65535.0;

vec3 auto_adaptation_apply_white_balance(
	vec3 color,
	vec4 wb_column_0,
	vec4 wb_column_1,
	vec4 wb_column_2)
{
	return mat3(wb_column_0.xyz, wb_column_1.xyz, wb_column_2.xyz) * color;
}

#endif
