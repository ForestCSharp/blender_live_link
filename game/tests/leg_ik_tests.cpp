// Solver-only checks for LegIK::solve on the Starter mech's left leg (bone heads
// from blend_files/Mechs/Starter/MechLegs.blend). No physics or Blender needed.
// Run from game/ with tests/run_leg_ik_tests.sh.
#define GAME_BUILD_CONFIG_NAME "LegIKTest"
#define main leg_ik_test_game_main
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

int main()
{
	test_target_at_animated_foot_changes_nothing();
	test_lower_target_is_reached_with_flat_foot();
	test_out_of_reach_target_straightens_without_nans();
	test_foot_follows_sloped_ground();
	puts("LEG_IK_TESTS_PASSED");
	fflush(stdout);
	// The engine was never initialized, so skip the game's global destructors.
	std::_Exit(0);
}
