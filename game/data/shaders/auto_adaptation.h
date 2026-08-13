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

vec3 auto_adaptation_apply_white_balance(
	vec3 color,
	vec4 wb_column_0,
	vec4 wb_column_1,
	vec4 wb_column_2)
{
	return mat3(wb_column_0.xyz, wb_column_1.xyz, wb_column_2.xyz) * color;
}

#endif
