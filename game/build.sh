
#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BASE_DIR="${SCRIPT_DIR##*/}"

cd $SCRIPT_DIR

lib=a

rm -rf bin
mkdir bin
mkdir bin/shaders

# Sokol Mac Build
clang -c src/sokol/sokol_single_file.c \
		-ObjC \
		-I src/sokol/ \
		-D SOKOL_METAL \
		-o bin/libsokol.$lib

# Sokol shader compile (Mac)
./tools/sokol-tools/bin/osx_arm64/sokol-shdc -i data/shaders/cube-sapp.glsl -o bin/shaders/cube-sapp.glsl.h --slang metal_macos

# Main Mac build
clang src/main.cpp \
	-o bin/game \
	-I bin/shaders \
	-I ../flatbuffers/include \
	-I ../compiled_schemas/cpp \
	--std=c++20 \
	-L./bin \
	-lsokol \
	-framework Cocoa \
	-framework Metal \
	-framework MetalKit \
	-framework Quartz 

# Run game
./bin/game
