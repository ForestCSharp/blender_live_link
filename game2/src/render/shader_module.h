#pragma once

#include "core/types.h"

#include <cstdio>
#include <cstdlib>

static bool read_binary_file(const char* in_filename, size_t* out_file_size, u32** out_data)
{
	FILE* file = fopen(in_filename, "rb");
	if (!file)
	{
		return false;
	}

	fseek(file, 0L, SEEK_END);
	const size_t file_size = *out_file_size = ftell(file);
	rewind(file);

	*out_data = (u32*) calloc(1, file_size + 1);
	if (!*out_data)
	{
		fclose(file);
		return false;
	}

	if (fread(*out_data, 1, file_size, file) != file_size)
	{
		fclose(file);
		free(*out_data);
		return false;
	}

	fclose(file);
	return true;
}

// Loads a glslc-compiled SPIR-V file (run from game2/ so bin/shaders/... resolves)
VkShaderModule create_shader_module_from_file(VkDevice in_device, const char* in_filename)
{
	size_t shader_size = 0;
	u32* shader_code = nullptr;
	if (!read_binary_file(in_filename, &shader_size, &shader_code))
	{
		printf("Failed to read shader file: %s (run from the game2 directory)\n", in_filename);
		exit(1);
	}

	VkShaderModuleCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shader_size,
		.pCode = shader_code,
	};

	VkShaderModule shader_module;
	VK_CHECK(vkCreateShaderModule(in_device, &create_info, nullptr, &shader_module));
	free(shader_code);

	return shader_module;
}
