#pragma once

#include "handmade_math/HandmadeMath.h"

struct Camera {
	HMM_Vec3 position;
	HMM_Vec3 forward;
	HMM_Vec3 up;
};

struct CameraControlSettings
{
	float follow_distance;
};

struct CameraControl
{
	Camera camera;
	float follow_distance;
};

CameraControl camera_control_create(const CameraControlSettings& in_settings)
{
	//FCS TODO: Init cam position to some value based on target object
	Camera camera = {
		.position = HMM_V3(0.0f, 0.0f, 0.0f),
		.forward = HMM_V3(0.0f, 1.0f, 0.0f),
		.up = HMM_V3(0.0f, 0.0f, 1.0f),
	};

	CameraControl out_camera_control = {
		.camera = camera,
		.follow_distance = in_settings.follow_distance,
	};

	return out_camera_control;
}
