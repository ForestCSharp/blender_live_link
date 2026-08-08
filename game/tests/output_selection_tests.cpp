#include <cassert>
#include <cstdio>
#include <cstring>
#include <vulkan/vulkan.h>

using u32 = unsigned int;

enum class EDisplayOutputMode
{
	SDR,
	EDR,
	HDR10,
};

#include "render/output_selection.inl"

int main()
{
	const VkSurfaceFormatKHR all_formats[] = {
		{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
		{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
		{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
	};
	DisplayOutputSelection selection = select_display_output(all_formats, 3, EDisplayOutputMode::HDR10);
	assert(selection.output_mode == EDisplayOutputMode::HDR10);
	assert(selection.surface_format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
	assert(selection.fallback_reason[0] == '\0');

	const VkSurfaceFormatKHR sdr_only[] = {
		{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	};
	selection = select_display_output(sdr_only, 1, EDisplayOutputMode::HDR10);
	assert(selection.output_mode == EDisplayOutputMode::SDR);
	assert(selection.surface_format.format == VK_FORMAT_B8G8R8A8_SRGB);
	assert(std::strstr(selection.fallback_reason, "HDR10_ST2084") != nullptr);

	selection = select_display_output(all_formats, 3, EDisplayOutputMode::SDR);
	assert(selection.output_mode == EDisplayOutputMode::SDR);
	assert(selection.surface_format.format == VK_FORMAT_B8G8R8A8_SRGB);

	selection = select_display_output(all_formats, 3, EDisplayOutputMode::EDR);
	assert(selection.output_mode == EDisplayOutputMode::EDR);
	assert(selection.surface_format.format == VK_FORMAT_R16G16B16A16_SFLOAT);
	return 0;
}
