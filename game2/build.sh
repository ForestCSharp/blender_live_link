#!/usr/bin/env bash

# game2 build: Vulkan (via MoltenVK on Mac) + Volk + VMA + GLFW. No CMake.
#
# Prerequisites:
#  - Vulkan SDK installed (headers in /usr/local/include, glslc on PATH,
#    MoltenVK ICD at /usr/local/share/vulkan/icd.d/MoltenVK_icd.json)
#  - ../compiled_schemas/cpp/blender_live_link_generated.h generated
#    (run the repo root ./build.sh once)
#
# Note: no Vulkan library is linked — volk dlopens the loader at runtime.
# Don't run game/ and game2/ at the same time; both listen on port 65432.

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

mkdir -p bin
mkdir -p bin/shaders

OS_ARG=$1
GAME_WARNING_FLAGS="-Wno-c99-designator"

./compile_shaders.sh $OS_ARG

echo OS in game2/build.sh is $OS_ARG

if [[ $OS_ARG = Mac ]]; then
	rm -rf bin/game

	# Compile GLFW as a static library, but only if it doesn't exist.
	# GLFW 3.4 can't be unity-built (the Cocoa and null backends share
	# static helper names), so each source file gets its own object.
	if [ ! -f ./bin/libglfw.a ]; then
		echo "libglfw.a not found, building"
		mkdir -p bin/glfw_obj
		GLFW_SOURCES="cocoa_init.m cocoa_joystick.m cocoa_monitor.m cocoa_time.c cocoa_window.m nsgl_context.m posix_module.c posix_thread.c context.c egl_context.c init.c input.c monitor.c null_init.c null_joystick.c null_monitor.c null_window.c osmesa_context.c platform.c vulkan.c window.c"
		for f in $GLFW_SOURCES; do
			clang -c src/extern/glfw/src/$f \
					-g -O0 \
					-ObjC \
					-D _GLFW_COCOA \
					-I src/extern/glfw/include \
					-o bin/glfw_obj/${f%.*}.o
		done
		ar rcs bin/libglfw.a bin/glfw_obj/*.o
	fi

	# Compile VMA as a static library, but only if it doesn't exist
	if [ ! -f ./bin/libvma.a ]; then
		echo "libvma.a not found, building"
		clang -c src/extern/vma/vma_single_file.cpp \
				-g -O0 \
				--std=c++20 \
				-Wno-everything \
				-o bin/libvma.a
	fi

	# Main Mac Build
	clang src/main.cpp \
		-g -O0 \
		$GAME_WARNING_FLAGS \
		-o bin/game \
		-I src \
		-I src/extern \
		-I src/extern/glfw/include \
		-I data \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		--std=c++20 \
		-L./bin \
		-lc++ \
		-lglfw \
		-lvma \
		-framework Cocoa \
		-framework IOKit \
		-framework QuartzCore

	if [ $? -ne 0 ]; then
		echo "game2 build failed"
		exit 1
	fi

	# Point volk's runtime loader at MoltenVK
	export VK_ICD_FILENAMES=/usr/local/share/vulkan/icd.d/MoltenVK_icd.json

	## Run game
	./bin/game
elif [[ $OS_ARG = Windows ]]; then
	# NOTE: drafted from game/build.sh's Windows branch + the reference
	# project's build.bat — not yet tested on a Windows machine.
	# Requires: clang + llvm-ar on PATH, VULKAN_SDK env var set by the SDK
	# installer, glslc on PATH.
	rm -rf bin/game.exe

	AR_BIN=$(command -v llvm-ar || command -v ar)

	# Compile GLFW as a static library, but only if it doesn't exist
	if [ ! -f ./bin/glfw.lib ]; then
		echo "glfw.lib not found, building"
		mkdir -p bin/glfw_obj
		GLFW_SOURCES="win32_init.c win32_joystick.c win32_module.c win32_monitor.c win32_thread.c win32_time.c win32_window.c wgl_context.c context.c egl_context.c init.c input.c monitor.c null_init.c null_joystick.c null_monitor.c null_window.c osmesa_context.c platform.c vulkan.c window.c"
		for f in $GLFW_SOURCES; do
			clang -c src/extern/glfw/src/$f \
					-g -O0 \
					-D _GLFW_WIN32 \
					-I src/extern/glfw/include \
					-o bin/glfw_obj/${f%.*}.o
		done
		"$AR_BIN" rcs bin/glfw.lib bin/glfw_obj/*.o
	fi

	# Compile VMA as a static library, but only if it doesn't exist
	if [ ! -f ./bin/vma.lib ]; then
		echo "vma.lib not found, building"
		clang -c src/extern/vma/vma_single_file.cpp \
				-g -O0 \
				--std=c++20 \
				-Wno-everything \
				-I "$VULKAN_SDK/Include" \
				-o bin/vma.lib
	fi

	# Main Windows Build
	clang src/main.cpp \
		-g -O0 \
		$GAME_WARNING_FLAGS \
		-o bin/game.exe \
		-I src \
		-I src/extern \
		-I src/extern/glfw/include \
		-I "$VULKAN_SDK/Include" \
		-I data \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		-D _CRT_SECURE_NO_WARNINGS \
		--std=c++20 \
		-L./bin \
		-lglfw \
		-lvma \
		-lUser32 \
		-lgdi32 \
		-lshell32

	if [ $? -ne 0 ]; then
		echo "game2 build failed"
		exit 1
	fi

	## Run game (volk finds vulkan-1.dll from the system loader install)
	./bin/game.exe
elif [[ $OS_ARG = Linux ]]; then
	echo "Building game2 for Linux: [TODO]"
else
	echo "Invalid OS Passed to game2/build.sh"
fi
