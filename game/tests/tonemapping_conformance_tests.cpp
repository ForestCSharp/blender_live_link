#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tonemapping_validation_common.h"

using TonemappingValidation::Assets;
using TonemappingValidation::Float4;
using TonemappingValidation::RGB;

static_assert(GT7Tonemapping::LUT_RESOLUTION == TONEMAP_LUT_RESOLUTION);
static_assert(ACES2Tonemapping::LUT_RESOLUTION == TONEMAP_LUT_RESOLUTION);
static_assert(AgXTonemapping::LUT_RESOLUTION == TONEMAP_LUT_RESOLUTION);
static_assert(ACES2Tonemapping::LUT_LAYER_OFFSET == TONEMAP_ACES2_LAYER_OFFSET);
static_assert(AgXTonemapping::LUT_LAYER_OFFSET == TONEMAP_AGX_LAYER_OFFSET);

struct Result
{
	std::string name;
	size_t samples = 0;
	float minimum = FLT_MAX;
	float maximum = -FLT_MAX;
	float maximum_neutral_error = 0.0f;
	float middle_gray = 0.0f;
};

static const char* method_name(int method)
{
	switch (method)
	{
		case TONEMAP_METHOD_GT7: return "gt7";
		case TONEMAP_METHOD_AGX: return "agx";
		case TONEMAP_METHOD_ACES_2: return "aces2";
		case TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL: return "khronos_pbr_neutral";
	}
	return "invalid";
}

static const char* output_name(int output_mode)
{
	return output_mode == 0 ? "sdr" : output_mode == 1 ? "edr" : "hdr10";
}

static RGB validation_linear_srgb_to_rec2020(RGB color)
{
	return {
		0.6274039f * color.r + 0.3292830f * color.g + 0.0433131f * color.b,
		0.0690973f * color.r + 0.9195404f * color.g + 0.0113623f * color.b,
		0.0163914f * color.r + 0.0880133f * color.g + 0.8955953f * color.b,
	};
}

static void expect_rgb_near(RGB actual, RGB expected, float tolerance)
{
	assert(std::abs(actual.r - expected.r) <= tolerance);
	assert(std::abs(actual.g - expected.g) <= tolerance);
	assert(std::abs(actual.b - expected.b) <= tolerance);
}

static Result validate_configuration(
	const Assets& assets,
	const std::vector<Float4>& corpus,
	int method,
	int output_mode,
	bool pbr_match_middle_gray)
{
	Result result;
	result.name = std::string(method_name(method)) + "/" + output_name(output_mode);
	if (method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
		result.name += pbr_match_middle_gray ? "/matched-gray" : "/exact";
	result.samples = corpus.size();

	for (const Float4& sample : corpus)
	{
		const RGB output = TonemappingValidation::sample_cpu(
			assets, method, output_mode, {sample.r, sample.g, sample.b}, pbr_match_middle_gray);
		assert(std::isfinite(output.r) && std::isfinite(output.g) && std::isfinite(output.b));
		result.minimum = std::min(result.minimum, std::min(output.r, std::min(output.g, output.b)));
		result.maximum = std::max(result.maximum, std::max(output.r, std::max(output.g, output.b)));
		if (output_mode == 0 || method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
		{
			assert(output.r >= -0.002f && output.r <= 1.002f);
			assert(output.g >= -0.002f && output.g <= 1.002f);
			assert(output.b >= -0.002f && output.b <= 1.002f);
		}
		else
		{
			const RGB rec2020 = validation_linear_srgb_to_rec2020(output);
			assert(rec2020.r >= -0.003f && rec2020.r <= 1.003f);
			assert(rec2020.g >= -0.003f && rec2020.g <= 1.003f);
			assert(rec2020.b >= -0.003f && rec2020.b <= 1.003f);
		}
	}

	const RGB negative = TonemappingValidation::sample_cpu(
		assets, method, output_mode, {-1.0f, 0.18f, 4.0f}, pbr_match_middle_gray);
	const RGB clamped = TonemappingValidation::sample_cpu(
		assets, method, output_mode, {0.0f, 0.18f, 4.0f}, pbr_match_middle_gray);
	expect_rgb_near(negative, clamped, 1e-6f);
	if (method != TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
	{
		const RGB at_limit = TonemappingValidation::sample_cpu(
			assets, method, output_mode, {64.0f, 4.0f, 0.5f}, false);
		const RGB above_limit = TonemappingValidation::sample_cpu(
			assets, method, output_mode, {65504.0f, 4.0f, 0.5f}, false);
		expect_rgb_near(at_limit, above_limit, 0.0f);
	}

	float previous = -1.0f;
	for (int step = 0; step <= 8192; ++step)
	{
		const float value = 64.0f * (float)step / 8192.0f;
		const RGB output = TonemappingValidation::sample_cpu(
			assets, method, output_mode, {value, value, value}, pbr_match_middle_gray);
		const float neutral = (output.r + output.g + output.b) / 3.0f;
		assert(neutral + 2e-4f >= previous);
		result.maximum_neutral_error = std::max(result.maximum_neutral_error,
			std::max(std::abs(output.r - output.g), std::abs(output.g - output.b)));
		previous = neutral;
	}

	const RGB gray = TonemappingValidation::sample_cpu(
		assets, method, output_mode, {0.18f, 0.18f, 0.18f}, pbr_match_middle_gray);
	result.middle_gray = (gray.r + gray.g + gray.b) / 3.0f;
	const float expected_gray = method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL
		? (pbr_match_middle_gray ? 0.214519f : 0.14f)
		: (output_mode == 0 ? 0.214519f : 0.203f);
	assert(std::abs(result.middle_gray - expected_gray) <= 0.001f);
	assert(result.maximum_neutral_error <= 0.003f);
	return result;
}

static void write_json(const char* path, const std::vector<Result>& results, size_t corpus_size)
{
	if (!path) return;
	const std::filesystem::path output_path(path);
	if (output_path.has_parent_path()) std::filesystem::create_directories(output_path.parent_path());
	std::ofstream file(output_path);
	assert(file.good());
	file << "{\n  \"suite\": \"tonemapping-cpu-conformance-v1\",\n";
	file << "  \"corpus_samples\": " << corpus_size << ",\n  \"passed\": true,\n";
	file << "  \"configurations\": [\n";
	for (size_t index = 0; index < results.size(); ++index)
	{
		const Result& result = results[index];
		file << "    {\"name\": \"" << result.name << "\", \"samples\": " << result.samples
			<< ", \"minimum\": " << result.minimum << ", \"maximum\": " << result.maximum
			<< ", \"middle_gray\": " << result.middle_gray
			<< ", \"maximum_neutral_error\": " << result.maximum_neutral_error << "}";
		file << (index + 1 == results.size() ? "\n" : ",\n");
	}
	file << "  ]\n}\n";
}

int main(int argc, char** argv)
{
	const char* json_path = argc == 3 && std::string(argv[1]) == "--json" ? argv[2] : nullptr;
	const std::vector<Float4> corpus = TonemappingValidation::make_corpus();
	assert(corpus.size() >= 100000);
	std::vector<Result> results;
	for (int output_mode = 0; output_mode < 3; ++output_mode)
	{
		Assets assets;
		std::string error;
		assert(TonemappingValidation::load_assets(output_mode, &assets, &error));
		for (int method = 0; method < TONEMAP_METHOD_COUNT; ++method)
		{
			results.push_back(validate_configuration(
				assets, corpus, method, output_mode, false));
			if (method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
				results.push_back(validate_configuration(
					assets, corpus, method, output_mode, true));
		}
	}
	write_json(json_path, results, corpus.size());
	std::printf("Tonemapping CPU conformance passed: %zu samples, %zu configurations\n",
		corpus.size(), results.size());
	return 0;
}
