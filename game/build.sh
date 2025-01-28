#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BASE_DIR="${SCRIPT_DIR##*/}"

cd $SCRIPT_DIR

#rm -rf bin
mkdir bin
mkdir bin/shaders

OS_ARG=$1

echo OS in game/build.sh is $OS_ARG

if [[ $OS_ARG = Windows ]]; then
	# Sokol Windows Build

	# Compile Sokol Library
	clang -c src/sokol/sokol_single_file.c \
			-I src/sokol/ \
			-D SOKOL_D3D11 \
			-o bin/sokol.lib


	# Compile jolt as library, but only if it doesn't exist 
	if [ ! -f ./bin/jolt.lib ]; then
		echo "jolt.lib not found, building now"
		clang 	-c src/Jolt/jolt_single_file.cpp \
				-o ./bin/jolt.lib \
				--std=c++20 \
				-I src 
	fi

	# Sokol shader compile (Windows)
	./tools/sokol-tools/bin/win32/sokol-shdc.exe -i data/shaders/basic_draw.glsl -o bin/shaders/basic_draw.compiled.h --slang hlsl5

	# Main Windows Build
	clang src/main.cpp \
		-g -gcodeview -O0 \
		-o bin/game.exe \
		-I src \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		--std=c++20 \
		-L./bin \
		-lsokol \
		-ljolt

	# Run game
	./bin/game.exe

elif [[ $OS_ARG = Mac ]]; then
	# Sokol Mac Build

	# Compile Sokol library
	clang -c src/sokol/sokol_single_file.c \
			-ObjC \
			-I src/sokol/ \
			-D SOKOL_METAL \
			-o bin/libsokol.a

	# Compile jolt as library, but only if it doesn't exist 
	if [ ! -f ./bin/libjolt.a ]; then
		echo "libjolt.a not found, building"
		clang 	-c src/Jolt/jolt_single_file.cpp \
				-o ./bin/libjolt.a \
				--std=c++20 \
				-I src 
	fi

	# Sokol shader compile (Mac)
	./tools/sokol-tools/bin/osx_arm64/sokol-shdc -i data/shaders/basic_draw.glsl -o bin/shaders/basic_draw.compiled.h --slang metal_macos

	# Main Mac Build
	clang src/main.cpp \
		-g -O0 \
		-o bin/game \
		-I src \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		--std=c++20 \
		-L./bin \
		-lc++ \
		-lsokol \
		-ljolt \
		-framework Cocoa \
		-framework Metal \
		-framework MetalKit \
		-framework Quartz 

	## Run game
	./bin/game
elif [[ $OS_ARG = Linux ]]; then
	echo "Building Game for Linux: [TODO]"
else
	echo "Invalid OS Passed to game/build.sh"
fi

