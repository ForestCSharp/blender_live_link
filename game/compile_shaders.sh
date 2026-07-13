#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

mkdir -p bin/shaders

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
			glslc $SHADER_OPT_FLAGS -I data/shaders --target-env=vulkan1.3 -DSHADOWS_ENABLED "$f" -o "$compiled_file"
		fi
	done < <(find data/shaders -name "*.$ext")
done

echo "$GAME_BUILD_CONFIG $SHADER_OPT_FLAGS" > "$CONFIG_STAMP"
