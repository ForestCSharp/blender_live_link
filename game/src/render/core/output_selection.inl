#pragma once

struct DisplayOutputSelection
{
	EDisplayOutputMode output_mode = EDisplayOutputMode::SDR;
	VkSurfaceFormatKHR surface_format = {};
	char fallback_reason[256] = {};
};

inline DisplayOutputSelection select_display_output(
	const VkSurfaceFormatKHR* in_formats,
	u32 in_format_count,
	EDisplayOutputMode in_requested_mode)
{
	DisplayOutputSelection result;
	auto find_format = [&](const VkFormat* preferred, u32 preferred_count,
		VkColorSpaceKHR color_space) -> bool
	{
		for (u32 preferred_index = 0; preferred_index < preferred_count; ++preferred_index)
		{
			for (u32 available_index = 0; available_index < in_format_count; ++available_index)
			{
				const VkSurfaceFormatKHR& available = in_formats[available_index];
				if (available.format == preferred[preferred_index]
					&& available.colorSpace == color_space)
				{
					result.surface_format = available;
					return true;
				}
			}
		}
		return false;
	};

	if (in_requested_mode == EDisplayOutputMode::EDR)
	{
		const VkFormat edr_formats[] = { VK_FORMAT_R16G16B16A16_SFLOAT };
		if (find_format(edr_formats, 1, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT))
			result.output_mode = EDisplayOutputMode::EDR;
		else
			snprintf(result.fallback_reason, sizeof(result.fallback_reason),
				"R16G16B16A16_SFLOAT + EXTENDED_SRGB_LINEAR was not advertised");
	}
	else if (in_requested_mode == EDisplayOutputMode::HDR10)
	{
		const VkFormat hdr10_formats[] = {
			VK_FORMAT_A2B10G10R10_UNORM_PACK32,
			VK_FORMAT_A2R10G10B10_UNORM_PACK32,
			VK_FORMAT_R16G16B16A16_SFLOAT,
		};
		if (find_format(hdr10_formats, 3, VK_COLOR_SPACE_HDR10_ST2084_EXT))
			result.output_mode = EDisplayOutputMode::HDR10;
		else
			snprintf(result.fallback_reason, sizeof(result.fallback_reason),
				"no supported HDR10_ST2084 surface pair was advertised");
	}

	if (result.output_mode == EDisplayOutputMode::SDR
		&& in_format_count == 1 && in_formats[0].format == VK_FORMAT_UNDEFINED)
	{
		result.surface_format = {
			.format = VK_FORMAT_B8G8R8A8_SRGB,
			.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		};
	}
	const VkFormat sdr_preferences[] = {
		VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
		VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
	};
	if (result.output_mode == EDisplayOutputMode::SDR
		&& result.surface_format.format == VK_FORMAT_UNDEFINED)
	{
		find_format(sdr_preferences, 4, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
	}
	if (result.output_mode == EDisplayOutputMode::SDR
		&& result.surface_format.format == VK_FORMAT_UNDEFINED)
	{
		// Never feed SDR values into an arbitrary HDR surface pair.
		for (u32 available_index = 0; available_index < in_format_count; ++available_index)
		{
			const VkSurfaceFormatKHR& available = in_formats[available_index];
			if (available.format != VK_FORMAT_UNDEFINED
				&& available.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				result.surface_format = available;
				break;
			}
		}
	}
	return result;
}
