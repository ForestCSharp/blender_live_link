
#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BASE_DIR="${SCRIPT_DIR##*/}"

cd $SCRIPT_DIR

rm -rf bin
mkdir bin
mkdir bin/shaders

if [[ $OS = Windows ]]; then
	# Sokol Windows Build
	clang -c src/sokol/sokol_single_file.c \
			-I src/sokol/ \
			-D SOKOL_D3D11 \
			-o bin/libsokol.lib

	# Sokol shader compile (Windows)
	./tools/sokol-tools/bin/win32/sokol-shdc.exe -i data/shaders/basic_draw.glsl -o bin/shaders/basic_draw.compiled.h --slang hlsl5

	# Main Windows Build
	clang src/main.cpp \
		-o bin/game.exe \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		--std=c++20 \
		-L./bin \
		-lc++ \
		-lsokol 

	# Run game
	./bin/game.exe

elif [[ $OS = Mac ]]; then
	# Sokol Mac Build
	clang -c src/sokol/sokol_single_file.c \
			-ObjC \
			-I src/sokol/ \
			-D SOKOL_METAL \
			-o bin/libsokol.a

	# Sokol shader compile (Mac)
	./tools/sokol-tools/bin/osx_arm64/sokol-shdc -i data/shaders/basic_draw.glsl -o bin/shaders/basic_draw.compiled.h --slang metal_macos

	# Main Mac Build
	clang src/main.cpp \
		-o bin/game \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		--std=c++20 \
		-L./bin \
		-lc++ \
		-lsokol \
		-framework Cocoa \
		-framework Metal \
		-framework MetalKit \
		-framework Quartz 

	# Run game
	./bin/game
elif [[ $OS = Linux ]]; then
	echo "Building Game for Linux: [TODO]"
else
	echo "Invalid OS Passed to game/build.sh"
fi
