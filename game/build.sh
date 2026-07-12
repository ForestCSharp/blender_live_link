#!/usr/bin/env bash

# game build: Vulkan (via MoltenVK on Mac) + Volk + VMA + GLFW. No CMake.
#
# Prerequisites:
#  - Vulkan SDK installed (headers in /usr/local/include, glslc on PATH,
#    MoltenVK ICD at /usr/local/share/vulkan/icd.d/MoltenVK_icd.json)
#  - ../compiled_schemas/cpp/blender_live_link_generated.h generated
#    (run the repo root ./build.sh once)
#
# Note: no Vulkan library is linked — volk dlopens the loader at runtime.
# Don't run game/ and game_old/ at the same time; both listen on port 65432.

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

mkdir -p bin
mkdir -p bin/shaders

OS_ARG=$1
# Pass -norun as the second arg to build without launching the game
# (used by automated verification runs)
RUN_ARG=$2
GAME_WARNING_FLAGS="-Wno-c99-designator"
WITH_DEBUG_UI=${WITH_DEBUG_UI:-1}
GAME_FEATURE_FLAGS="-D WITH_DEBUG_UI=$WITH_DEBUG_UI"

./compile_shaders.sh $OS_ARG

echo OS in game/build.sh is $OS_ARG

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

	# Compile Jolt as a static library, but only if it doesn't exist.
	# IMPORTANT: keep the JPH_* define set EMPTY and identical between this
	# compile and the main build below — mismatched defines break Jolt's ABI.
	if [ ! -f ./bin/libjolt.a ]; then
		echo "libjolt.a not found, building"
		clang -c src/extern/Jolt/jolt_single_file.cpp \
				--std=c++20 \
				-I src/extern \
				-o bin/libjolt.a
	fi

	# Main Mac Build
	clang src/main.cpp \
		-g -O0 \
		$GAME_WARNING_FLAGS \
		$GAME_FEATURE_FLAGS \
		-o bin/game \
		-I src \
		-I src/extern \
		-I src/extern/glfw/include \
		-I ../game_old/src/extern/imgui \
		-I ../game_old/src/extern \
		-I data \
		-I data/shaders \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		--std=c++20 \
		-L./bin \
		-lc++ \
		-lglfw \
		-lvma \
		-ljolt \
		-framework Cocoa \
		-framework IOKit \
		-framework QuartzCore

	if [ $? -ne 0 ]; then
		echo "game build failed"
		exit 1
	fi

	# Point volk's runtime loader at MoltenVK
	export VK_ICD_FILENAMES=/usr/local/share/vulkan/icd.d/MoltenVK_icd.json

	## Run game
	if [[ $RUN_ARG != -norun ]]; then
		./bin/game
	fi
elif [[ $OS_ARG = Windows ]]; then
	# NOTE: drafted from game_old/build.sh's Windows branch + the reference
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

	# Compile Jolt as a static library, but only if it doesn't exist
	# (NOTE: correct src/extern/Jolt path — game_old/'s Windows branch still
	# points at a stale location. Keep JPH_* defines empty on both compiles.)
	if [ ! -f ./bin/jolt.lib ]; then
		echo "jolt.lib not found, building"
		clang -c src/extern/Jolt/jolt_single_file.cpp \
				--std=c++20 \
				-I src/extern \
				-o bin/jolt.lib
	fi

	# Main Windows Build
	clang src/main.cpp \
		-g -O0 \
		$GAME_WARNING_FLAGS \
		$GAME_FEATURE_FLAGS \
		-o bin/game.exe \
		-I src \
		-I src/extern \
		-I src/extern/glfw/include \
		-I ../game_old/src/extern/imgui \
		-I ../game_old/src/extern \
		-I "$VULKAN_SDK/Include" \
		-I data \
		-I data/shaders \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		-D _CRT_SECURE_NO_WARNINGS \
		--std=c++20 \
		-L./bin \
		-lglfw \
		-lvma \
		-ljolt \
		-lUser32 \
		-lgdi32 \
		-lshell32

	if [ $? -ne 0 ]; then
		echo "game build failed"
		exit 1
	fi

	## Run game (volk finds vulkan-1.dll from the system loader install)
	if [[ $RUN_ARG != -norun ]]; then
		./bin/game.exe
	fi
elif [[ $OS_ARG = Linux ]]; then
	echo "Building game for Linux: [TODO]"
else
	echo "Invalid OS Passed to game/build.sh"
fi
