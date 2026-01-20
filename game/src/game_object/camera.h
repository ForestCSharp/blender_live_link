#pragma once

#include "handmade_math/HandmadeMath.h"

struct Camera {
	HMM_Vec3 location;
	HMM_Vec3 forward;
	HMM_Vec3 up;
};

struct CameraControlSettings
{
	HMM_Vec3 initial_location;
	HMM_Vec3 initial_direction;
	float follow_distance;
	float follow_speed;
};

struct CameraControl
{
	Camera camera;
	float follow_distance;
	float follow_speed;
};

CameraControl camera_control_create(const CameraControlSettings& in_settings)
{
	Camera camera = {
		.location = in_settings.initial_location,
		.forward = in_settings.initial_direction,
		.up = HMM_V3(0.0f, 0.0f, 1.0f),
	};

	CameraControl out_camera_control = {
		.camera = camera,
		.follow_distance = in_settings.follow_distance,
		.follow_speed = in_settings.follow_speed,
	};

	return out_camera_control;
}

HMM_Vec3 camera_control_get_desired_location(
	const HMM_Vec3& in_target_location, 
	const HMM_Vec3& in_direction,
	float in_follow_distance
)
{
	return in_target_location - (in_direction * in_follow_distance);
}

