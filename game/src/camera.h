#pragma once

#include "handmade_math/HandmadeMath.h"

struct Camera {
	HMM_Vec3 position;
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
		.position = in_settings.initial_location,
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
