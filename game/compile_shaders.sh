#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

mkdir -p bin/shaders
VULKAN_SDK_PATH=${VULKAN_SDK:-}
if [[ -n "$VULKAN_SDK_PATH" ]] && command -v cygpath > /dev/null 2>&1; then
	VULKAN_SDK_PATH=$(cygpath -u "$VULKAN_SDK_PATH")
fi

resolve_glslc() {
	if [[ -n "${GLSLC:-}" ]]; then
		printf "%s\n" "$GLSLC"
		return
	fi

	if [[ -n "$VULKAN_SDK_PATH" ]]; then
		local candidate
		for candidate in \
			"$VULKAN_SDK_PATH/bin/glslc" \
			"$VULKAN_SDK_PATH/Bin/glslc.exe" \
			"$VULKAN_SDK_PATH/bin/glslc.exe"; do
			if [[ -x "$candidate" ]]; then
				printf "%s\n" "$candidate"
				return
			fi
		done
	fi

	command -v glslc || true
}

GLSLC_BINARY=$(resolve_glslc)
if [[ -z "$GLSLC_BINARY" ]]; then
	echo "Error: glslc was not found. Install the Vulkan SDK, set VULKAN_SDK, or set GLSLC."
	exit 1
fi

GAME_BUILD_CONFIG=${GAME_BUILD_CONFIG:-Develop}
SHADER_OPT_FLAGS=${SHADER_OPT_FLAGS:--O -g}
CONFIG_STAMP="bin/shaders/.build_config"
PREVIOUS_CONFIG=""
if [ -f "$CONFIG_STAMP" ]; then
	PREVIOUS_CONFIG=$(<"$CONFIG_STAMP")
fi
REBUILD_ALL=0
if [ "$PREVIOUS_CONFIG" != "$GAME_BUILD_CONFIG $SHADER_OPT_FLAGS" ]; then
	REBUILD_ALL=1
fi

# Compile GLSL -> SPIR-V with glslc (from the Vulkan SDK).
# forward.vert -> bin/shaders/forward.vert.spv
for ext in vert frag comp; do
	while read -r f; do
		file_name=$(basename "$f")
		compiled_file="bin/shaders/$file_name.spv"
		NEEDS_REBUILD=$REBUILD_ALL
		if [ ! -f "$compiled_file" ] || [ "$f" -nt "$compiled_file" ]; then NEEDS_REBUILD=1; fi
		for include_file in data/shaders/*.h; do
			if [ "$include_file" -nt "$compiled_file" ]; then NEEDS_REBUILD=1; fi
		done
		if [ $NEEDS_REBUILD -eq 1 ]; then
			echo "compiling $f to $compiled_file ($GAME_BUILD_CONFIG)"
			"$GLSLC_BINARY" $SHADER_OPT_FLAGS -I data/shaders --target-env=vulkan1.3 -DSHADOWS_ENABLED "$f" -o "$compiled_file"
		fi
	done < <(find data/shaders -name "*.$ext")
done

echo "$GAME_BUILD_CONFIG $SHADER_OPT_FLAGS" > "$CONFIG_STAMP"
