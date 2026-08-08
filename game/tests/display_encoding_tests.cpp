#include <algorithm>
#include <cassert>
#include <cmath>

struct RGB
{
	double r;
	double g;
	double b;
};

static RGB linear_srgb_to_rec2020(RGB color)
{
	return {
		0.6274039 * color.r + 0.3292830 * color.g + 0.0433131 * color.b,
		0.0690973 * color.r + 0.9195404 * color.g + 0.0113623 * color.b,
		0.0163914 * color.r + 0.0880133 * color.g + 0.8955953 * color.b,
	};
}

static double st2084_from_nits(double nits)
{
	constexpr double m1 = 0.1593017578125;
	constexpr double m2 = 78.84375;
	constexpr double c1 = 0.8359375;
	constexpr double c2 = 18.8515625;
	constexpr double c3 = 18.6875;
	const double normalized = std::clamp(nits / 10000.0, 0.0, 1.0);
	const double powered = std::pow(normalized, m1);
	return std::pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}

static void expect_near(double actual, double expected, double tolerance)
{
	assert(std::abs(actual - expected) <= tolerance);
}

int main()
{
	const RGB red = linear_srgb_to_rec2020({1.0, 0.0, 0.0});
	expect_near(red.r, 0.6274039, 1e-7);
	expect_near(red.g, 0.0690973, 1e-7);
	expect_near(red.b, 0.0163914, 1e-7);
	const RGB neutral = linear_srgb_to_rec2020({1.0, 1.0, 1.0});
	expect_near(neutral.r, 1.0, 1e-6);
	expect_near(neutral.g, 1.0, 1e-6);
	expect_near(neutral.b, 1.0, 1e-6);

	// ST-2084's exact mathematical black code is a tiny positive value; a
	// 10-bit UNORM surface quantizes it to code zero.
	expect_near(st2084_from_nits(0.0), 0.0000007309559, 1e-12);
	expect_near(st2084_from_nits(203.0), 0.5806889, 1e-6);
	expect_near(st2084_from_nits(1000.0), 0.7518271, 1e-6);

	constexpr double edr_scale = 1000.0 / 203.0;
	expect_near(0.203 * edr_scale, 1.0, 1e-12);
	expect_near(1.0 * edr_scale, 4.926108374384236, 1e-12);
	return 0;
}
