#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

mkdir -p bin/shaders

# Compile GLSL -> SPIR-V with glslc (from the Vulkan SDK).
# forward.vert -> bin/shaders/forward.vert.spv
for ext in vert frag comp; do
	find data/shaders -name "*.$ext" | while read f; do
		file_name=$(basename $f)
		compiled_file="bin/shaders/$file_name.spv"
		echo "compiling $f to $compiled_file"
		glslc "$f" -o "$compiled_file"
	done
done
