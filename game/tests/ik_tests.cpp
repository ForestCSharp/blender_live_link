// Solver-only checks for ArmIK::solve and LegIK::solve. The leg uses the Starter
// mech's left leg (bone heads from blend_files/Mechs/Starter/MechLegs.blend).
// No physics or Blender needed. Run from game/ with tests/run_ik_tests.sh.
#define GAME_BUILD_CONFIG_NAME "IKTest"
#define main ik_test_game_main
#include "../src/main.cpp"
#undef main
#include <cassert>

static bool near(HMM_Vec3 a, HMM_Vec3 b, f32 tolerance = 1e-3f)
{
	return HMM_LenV3(a - b) < tolerance;
}

// Pelvis -> Thigh -> Shin -> Hock -> Foot, rest pose == animated pose, identity bone rotations.
struct TestLeg
{
	ArmatureBone bones[5];
	Armature rig;
	LegIK::Chain chain = {1, 2, 3, 4};

	TestLeg()
	{
		const HMM_Vec3 heads[5] = {
			HMM_V3(0.0f, 0.0f, 0.14f),
			HMM_V3(-0.5f, 0.0f, 0.015f),
			HMM_V3(-0.5f, 0.325f, -0.335f),
			HMM_V3(-0.5f, -0.36f, -0.71f),
			HMM_V3(-0.5f, 0.05f, -1.575f),
		};
		for (i32 i = 0; i < 5; ++i)
		{
			bones[i].parent_index = i - 1;
			bones[i].inverse_bind_matrix = HMM_Translate(-heads[i]);
			rig.evaluated_pose.push_back(HMM_Translate(heads[i]));
		}
		rig.bone_count = 5;
		rig.bones = bones;
	}

	HMM_Vec3 joint(i32 in_bone) const { return rig.evaluated_pose[in_bone].Columns[3].XYZ; }
};

static const HMM_Vec3 up = HMM_V3(0, 0, 1);
static const HMM_Vec3 forward = HMM_V3(0, 1, 0);

static void test_target_at_animated_foot_changes_nothing()
{
	TestLeg leg;
	const TestLeg before;
	std::string error;
	assert(LegIK::solve(leg.rig, leg.chain, HMM_M4D(1.0f), before.joint(4), up, forward, error));
	for (i32 bone = 0; bone < 5; ++bone)
	{
		assert(near(leg.joint(bone), before.joint(bone)));
	}
}

static void test_lower_target_is_reached_with_flat_foot()
{
	TestLeg leg;
	const HMM_Vec3 target = leg.joint(4) - HMM_V3(0, 0, 0.5f);
	std::string error;
	assert(LegIK::solve(leg.rig, leg.chain, HMM_M4D(1.0f), target, up, forward, error));
	assert(near(leg.joint(4), target));
	// Rest foot rotation is identity, so a flat foot keeps identity axes.
	assert(near(leg.rig.evaluated_pose[4].Columns[0].XYZ, HMM_V3(1, 0, 0)));
	assert(near(leg.rig.evaluated_pose[4].Columns[2].XYZ, up));
}

static void test_out_of_reach_target_straightens_without_nans()
{
	TestLeg leg;
	const HMM_Vec3 hip = leg.joint(1);
	const HMM_Vec3 target = leg.joint(4) - HMM_V3(0, 0, 2.0f);
	std::string error;
	assert(LegIK::solve(leg.rig, leg.chain, HMM_M4D(1.0f), target, up, forward, error));
	// Fully extended: hip-to-foot equals thigh + shin + hock.
	const f32 full_length = HMM_LenV3(leg.joint(2) - hip) + HMM_LenV3(leg.joint(3) - leg.joint(2)) + HMM_LenV3(leg.joint(4) - leg.joint(3));
	assert(fabsf(HMM_LenV3(leg.joint(4) - hip) - full_length) < 1e-2f);
}

static void test_foot_follows_sloped_ground()
{
	TestLeg leg;
	const HMM_Vec3 slope = HMM_NormV3(HMM_V3(0, 0.3f, 1));
	std::string error;
	assert(LegIK::solve(leg.rig, leg.chain, HMM_M4D(1.0f), leg.joint(4), slope, forward, error));
	assert(near(leg.rig.evaluated_pose[4].Columns[2].XYZ, slope));
}

// Root -> Shoulder -> Elbow -> Hand, identity bone rotations; in_hand sets the rest bend.
struct TestArm
{
	ArmatureBone bones[4];
	Armature rig;
	ArmIK::Chain chain = {1, 2, 3};

	explicit TestArm(HMM_Vec3 in_hand = HMM_V3(0.5f, 0.7f, 0.9f))
	{
		const HMM_Vec3 heads[4] = {HMM_V3(0, 0, 1), HMM_V3(0.5f, 0, 1), HMM_V3(0.5f, 0.4f, 0.8f), in_hand};
		for (i32 i = 0; i < 4; ++i)
		{
			bones[i].parent_index = i - 1;
			bones[i].inverse_bind_matrix = HMM_Translate(-heads[i]);
			rig.evaluated_pose.push_back(HMM_Translate(heads[i]));
		}
		rig.bone_count = 4;
		rig.bones = bones;
	}

	HMM_Vec3 joint(i32 in_bone) const { return rig.evaluated_pose[in_bone].Columns[3].XYZ; }
};

static f32 elbow_bend(const TestArm& in_arm)
{
	const HMM_Vec3 upper = in_arm.joint(2) - in_arm.joint(1);
	const HMM_Vec3 lower = in_arm.joint(3) - in_arm.joint(2);
	return acosf(HMM_Clamp(-1.0f, HMM_DotV3(upper, lower) / (HMM_LenV3(upper) * HMM_LenV3(lower)), 1.0f));
}

static void test_arm_aims_hand_and_muzzle_along_view()
{
	TestArm arm;
	const TestArm before;
	const HMM_Vec3 aim = HMM_NormV3(HMM_V3(0.3f, 1.0f, 0.2f));
	std::string error;
	assert(ArmIK::solve(arm.rig, arm.chain, HMM_M4D(1.0f), aim, HMM_V3(0, 1, 0), error));
	// Lengths kept, hand on the aim line from the shoulder, elbow bent at least the minimum.
	assert(fabsf(HMM_LenV3(arm.joint(2) - arm.joint(1)) - HMM_LenV3(before.joint(2) - before.joint(1))) < 1e-3f);
	assert(fabsf(HMM_LenV3(arm.joint(3) - arm.joint(2)) - HMM_LenV3(before.joint(3) - before.joint(2))) < 1e-3f);
	const HMM_Vec3 to_hand = arm.joint(3) - arm.joint(1);
	assert(near(HMM_NormV3(to_hand), aim));
	assert(elbow_bend(arm) >= HMM_AngleDeg(ArmIK::min_elbow_bend_degrees) - 1e-3f);
	// Hand +Y (the default muzzle axis) points along the view.
	assert(near(HMM_NormV3(arm.rig.evaluated_pose[3].Columns[1].XYZ), aim));
}

static void test_straight_arm_bends_elbow_down()
{
	TestArm arm(HMM_V3(0.5f, 0.8f, 0.6f));	// Collinear with shoulder -> elbow.
	const HMM_Vec3 aim = HMM_V3(0, 1, 0);
	std::string error;
	assert(ArmIK::solve(arm.rig, arm.chain, HMM_M4D(1.0f), aim, HMM_V3(0, 1, 0), error));
	assert(arm.joint(2).Z < arm.joint(1).Z - 1e-3f);
}

// Exact output for awkward inputs (rotated, scaled rig; diagonal aim; slope),
// recorded before the ik_common.h refactor so it must stay behavior-identical.
static HMM_Mat4 golden_rig_world()
{
	return HMM_Translate(HMM_V3(3, -2, 5)) * HMM_QToM4(HMM_QFromAxisAngle_RH(HMM_NormV3(HMM_V3(0.2f, 0.1f, 1)), 0.7f)) * HMM_Scale(HMM_V3(1.5f, 1.5f, 1.5f));
}

static void check_golden(const Armature& in_rig, const f32* in_expected)
{
	for (u32 bone = 0; bone < in_rig.bone_count; ++bone)
	{
		for (i32 element = 0; element < 16; ++element)
		{
			assert(fabsf(in_rig.evaluated_pose[bone].Elements[element / 4][element % 4] - in_expected[bone * 16 + element]) < 1e-4f);
		}
	}
}

static void test_golden_outputs_unchanged()
{
	static const f32 arm_expected[] = {
		1.000000f, 0.000000f, 0.000000f, 0.000000f,
		0.000000f, 1.000000f, 0.000000f, 0.000000f,
		0.000000f, 0.000000f, 1.000000f, 0.000000f,
		0.000000f, 0.000000f, 1.000000f, 1.000000f,
		0.954808f, -0.285527f, 0.082558f, 0.000000f,
		0.297067f, 0.907751f, -0.296208f, 0.000000f,
		0.009634f, 0.307347f, 0.951549f, 0.000000f,
		0.500000f, 0.000000f, 1.000000f, 1.000000f,
		0.954808f, -0.285527f, 0.082558f, 0.000000f,
		0.296776f, 0.931084f, -0.212148f, 0.000000f,
		-0.016294f, 0.227062f, 0.973744f, 0.000000f,
		0.616900f, 0.301631f, 0.691207f, 1.000000f,
		0.954808f, -0.285527f, 0.082558f, 0.000000f,
		0.289882f, 0.833232f, -0.470843f, 0.000000f,
		0.065649f, 0.473496f, 0.878346f, 0.000000f,
		0.704303f, 0.603662f, 0.724937f, 1.000000f,
	};
	static const f32 leg_expected[] = {
		1.000000f, 0.000000f, 0.000000f, 0.000000f,
		0.000000f, 1.000000f, 0.000000f, 0.000000f,
		0.000000f, 0.000000f, 1.000000f, 0.000000f,
		0.000000f, 0.000000f, 0.140000f, 1.000000f,
		0.999993f, 0.000862f, 0.003638f, 0.000000f,
		-0.001224f, 0.994927f, 0.100593f, 0.000000f,
		-0.003533f, -0.100597f, 0.994921f, 0.000000f,
		-0.500000f, 0.000000f, 0.015000f, 1.000000f,
		0.999993f, 0.000862f, 0.003638f, 0.000000f,
		-0.002731f, 0.833100f, 0.553116f, 0.000000f,
		-0.002554f, -0.553122f, 0.833096f, 0.000000f,
		-0.499161f, 0.358560f, -0.300529f, 1.000000f,
		0.999993f, 0.000862f, 0.003638f, 0.000000f,
		0.000796f, 0.901625f, -0.432518f, 0.000000f,
		-0.003653f, 0.432518f, 0.901618f, 0.000000f,
		-0.496333f, -0.004692f, -0.991825f, 1.000000f,
		0.998731f, -0.004925f, 0.050111f, 0.000000f,
		-0.004925f, 0.980876f, 0.194572f, 0.000000f,
		-0.050111f, -0.194572f, 0.979607f, 0.000000f,
		-0.492847f, -0.009154f, -1.949057f, 1.000000f,
	};
	TestArm arm;
	std::string error;
	assert(ArmIK::solve(arm.rig, arm.chain, golden_rig_world(), HMM_NormV3(HMM_V3(-0.4f, 1, -0.3f)), HMM_V3(0, 1.2f, 0.1f), error));
	check_golden(arm.rig, arm_expected);
	// Recorded at 0.8 extension, which also exercises the hock-straightening bisection.
	state.character.leg_ik_max_extension_scale = 0.8f;
	TestLeg leg;
	const HMM_Vec3 target = (golden_rig_world() * HMM_V4V(leg.joint(4), 1)).XYZ - HMM_V3(0, 0, 0.6f);
	assert(LegIK::solve(leg.rig, leg.chain, golden_rig_world(), target, HMM_NormV3(HMM_V3(0.2f, -0.3f, 1)), HMM_V3(0.1f, 1, 0), error));
	state.character.leg_ik_max_extension_scale = 1.0f;
	check_golden(leg.rig, leg_expected);
}

int main()
{
	test_arm_aims_hand_and_muzzle_along_view();
	test_straight_arm_bends_elbow_down();
	// Reach expectations assume the full natural extension, not the UI default.
	state.character.leg_ik_max_extension_scale = 1.0f;
	test_target_at_animated_foot_changes_nothing();
	test_lower_target_is_reached_with_flat_foot();
	test_out_of_reach_target_straightens_without_nans();
	test_foot_follows_sloped_ground();
	test_golden_outputs_unchanged();
	puts("IK_TESTS_PASSED");
	fflush(stdout);
	// The engine was never initialized, so skip the game's global destructors.
	std::_Exit(0);
}
