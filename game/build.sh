#!/usr/bin/env bash
set -o pipefail

# game build: Vulkan (via MoltenVK on Mac) + Volk + VMA + GLFW. No CMake.
#
# Prerequisites:
#  - Vulkan SDK installed (set VULKAN_SDK or place headers/tools under
#    /usr/local; glslc must be available through the SDK, GLSLC, or PATH)
#  - ../compiled_schemas/cpp/blender_live_link_generated.h generated
#    (run the repo root ./build.sh once)
#
# Note: no Vulkan library is linked — volk dlopens the loader at runtime.
# Don't run game/ and game_old/ at the same time; both listen on port 65432.

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

mkdir -p bin
mkdir -p bin/shaders

usage() {
	echo "Usage: ./build.sh <Mac|Linux|Windows> [-norun] [-full]"
}

if [ $# -lt 1 ]; then
	usage
	exit 1
fi

OS_ARG=$1
shift
RUN_GAME=1
FULL_REBUILD=0
for arg in "$@"; do
	case "$arg" in
		-norun)
			RUN_GAME=0
			;;
		-full)
			FULL_REBUILD=1
			;;
		*)
			echo "Unknown argument: $arg"
			usage
			exit 1
			;;
	esac
done

case "$OS_ARG" in
	Mac|Linux|Windows)
		;;
	*)
		echo "Invalid OS: $OS_ARG"
		usage
		exit 1
		;;
esac

GAME_WARNING_FLAGS="-Wno-c99-designator"
WITH_DEBUG_UI=${WITH_DEBUG_UI:-1}
GAME_BUILD_CONFIG=${GAME_BUILD_CONFIG:-Debug}
VULKAN_INCLUDE_ARGS=()
VULKAN_SDK_PATH=${VULKAN_SDK:-}
if [[ -n "$VULKAN_SDK_PATH" ]] && command -v cygpath > /dev/null 2>&1; then
	VULKAN_SDK_PATH=$(cygpath -u "$VULKAN_SDK_PATH")
fi

resolve_vulkan_include_args() {
	if [[ -z "$VULKAN_SDK_PATH" ]]; then
		return
	fi

	local include_dir=""
	if [[ -f "$VULKAN_SDK_PATH/include/vulkan/vulkan.h" ]]; then
		include_dir="$VULKAN_SDK_PATH/include"
	elif [[ -f "$VULKAN_SDK_PATH/Include/vulkan/vulkan.h" ]]; then
		include_dir="$VULKAN_SDK_PATH/Include"
	fi

	if [[ -z "$include_dir" ]]; then
		echo "Error: VULKAN_SDK is set but vulkan/vulkan.h was not found beneath it: $VULKAN_SDK_PATH"
		exit 1
	fi

	VULKAN_INCLUDE_ARGS=(-I "$include_dir")
}

configure_moltenvk_runtime() {
	if [[ -n "${VK_ICD_FILENAMES:-}" || -n "${VK_DRIVER_FILES:-}" ]]; then
		return
	fi

	local candidate
	for candidate in \
		"${VULKAN_SDK_PATH}/share/vulkan/icd.d/MoltenVK_icd.json" \
		"${VULKAN_SDK_PATH}/etc/vulkan/icd.d/MoltenVK_icd.json" \
		"/usr/local/share/vulkan/icd.d/MoltenVK_icd.json"; do
		if [[ -f "$candidate" ]]; then
			export VK_ICD_FILENAMES="$candidate"
			return
		fi
	done

	echo "Warning: MoltenVK ICD manifest was not found; Vulkan loader discovery will use its defaults."
}

resolve_vulkan_include_args

case "$GAME_BUILD_CONFIG" in
	Debug)
		GAME_OPT_FLAGS="-O0 -g"
		SHADER_OPT_FLAGS="-O0 -g"
		DEFAULT_VALIDATION=1
		;;
	Develop)
		GAME_OPT_FLAGS="-O2 -g"
		SHADER_OPT_FLAGS="-O -g"
		DEFAULT_VALIDATION=1
		;;
	Release)
		GAME_OPT_FLAGS="-O3 -DNDEBUG"
		SHADER_OPT_FLAGS="-O"
		DEFAULT_VALIDATION=0
		;;
	*)
		echo "Invalid GAME_BUILD_CONFIG '$GAME_BUILD_CONFIG' (expected Debug, Develop, or Release)"
		exit 1
		;;
esac

GAME_ENABLE_VALIDATION=${GAME_ENABLE_VALIDATION:-$DEFAULT_VALIDATION}
GAME_FEATURE_FLAGS="-D WITH_DEBUG_UI=$WITH_DEBUG_UI -D GAME_ENABLE_VALIDATION=$GAME_ENABLE_VALIDATION -D GAME_BUILD_CONFIG_NAME=\"$GAME_BUILD_CONFIG\""
BUILD_CACHE_DIR="bin/build/$OS_ARG/$GAME_BUILD_CONFIG"
if [ $FULL_REBUILD -eq 1 ]; then
	echo "Clearing cached dependencies for $OS_ARG/$GAME_BUILD_CONFIG"
	rm -rf "$BUILD_CACHE_DIR"
fi
mkdir -p "$BUILD_CACHE_DIR"

generate_dependency_manifest() {
	local source_dir=$1
	local output_file=$2
	{
		echo "dependency-manifest-v1"
		find "$source_dir" -type f -exec cksum {} + | LC_ALL=C sort -k 3
	} > "$output_file"
}

run_timed() {
	local label=$1
	shift
	local previous_timeformat=${TIMEFORMAT-}
	local status
	TIMEFORMAT="[timing] $label: %3R s"
	time "$@"
	status=$?
	TIMEFORMAT=$previous_timeformat
	return $status
}

dependency_needs_rebuild() {
	local dependency_name=$1
	local source_dir=$2
	local artifact=$3
	local manifest="$BUILD_CACHE_DIR/$dependency_name.manifest"
	local candidate="$manifest.next"

	if ! run_timed "$dependency_name manifest scan" generate_dependency_manifest "$source_dir" "$candidate"; then
		echo "Failed to generate dependency manifest for $dependency_name"
		exit 1
	fi
	if [ ! -f "$artifact" ] || [ ! -f "$manifest" ] || ! cmp -s "$manifest" "$candidate"; then
		return 0
	fi

	rm -f "$candidate"
	return 1
}

commit_dependency_manifest() {
	local dependency_name=$1
	mv "$BUILD_CACHE_DIR/$dependency_name.manifest.next" "$BUILD_CACHE_DIR/$dependency_name.manifest" || exit 1
}

export GAME_BUILD_CONFIG
export SHADER_OPT_FLAGS

if [ $FULL_REBUILD -eq 1 ]; then
	run_timed "shader cache/build" ./compile_shaders.sh "$OS_ARG" -full || exit 1
else
	run_timed "shader cache/build" ./compile_shaders.sh "$OS_ARG" || exit 1
fi

echo "Building game for $OS_ARG ($GAME_BUILD_CONFIG, validation=$GAME_ENABLE_VALIDATION, debug_ui=$WITH_DEBUG_UI)"

if [[ $OS_ARG = Mac ]]; then
	rm -rf bin/game

	# Compile GLFW as a static library when its vendored directory changes.
	# GLFW 3.4 can't be unity-built (the Cocoa and null backends share
	# static helper names), so each source file gets its own object.
	GLFW_LIBRARY="$BUILD_CACHE_DIR/libglfw.a"
	GLFW_OBJECT_DIR="$BUILD_CACHE_DIR/glfw_obj"
	GLFW_SOURCES="cocoa_init.m cocoa_joystick.m cocoa_monitor.m cocoa_time.c cocoa_window.m nsgl_context.m posix_module.c posix_thread.c context.c egl_context.c init.c input.c monitor.c null_init.c null_joystick.c null_monitor.c null_window.c osmesa_context.c platform.c vulkan.c window.c"
	if dependency_needs_rebuild glfw extern/glfw "$GLFW_LIBRARY"; then
		echo "Building GLFW ($GAME_BUILD_CONFIG)"
		TIMEFORMAT="[timing] GLFW build: %3R s"
		time {
			rm -rf "$GLFW_OBJECT_DIR"
			mkdir -p "$GLFW_OBJECT_DIR"
			for f in $GLFW_SOURCES; do
				clang -c extern/glfw/src/$f \
						$GAME_OPT_FLAGS \
						-ObjC \
						-D _GLFW_COCOA \
						-I extern/glfw/include \
						-o "$GLFW_OBJECT_DIR/${f%.*}.o" || exit 1
			done
			ar rcs "$GLFW_LIBRARY" "$GLFW_OBJECT_DIR"/*.o || exit 1
		}
		commit_dependency_manifest glfw
	else
		echo "Using cached GLFW ($GAME_BUILD_CONFIG)"
	fi

	# Compile VMA as a static library when its vendored directory changes.
	VMA_LIBRARY="$BUILD_CACHE_DIR/libvma.a"
	if dependency_needs_rebuild vma extern/vma "$VMA_LIBRARY"; then
		echo "Building VMA ($GAME_BUILD_CONFIG)"
		run_timed "VMA build" clang -c extern/vma/vma_single_file.cpp \
				$GAME_OPT_FLAGS \
				--std=c++20 \
				-Wno-everything \
				"${VULKAN_INCLUDE_ARGS[@]}" \
				-o "$VMA_LIBRARY" || exit 1
		commit_dependency_manifest vma
	else
		echo "Using cached VMA ($GAME_BUILD_CONFIG)"
	fi

	# Compile Jolt as a static library when its vendored directory changes.
	# IMPORTANT: keep the JPH_* define set EMPTY and identical between this
	# compile and the main build below — mismatched defines break Jolt's ABI.
	JOLT_LIBRARY="$BUILD_CACHE_DIR/libjolt.a"
	if dependency_needs_rebuild jolt extern/Jolt "$JOLT_LIBRARY"; then
		echo "Building Jolt ($GAME_BUILD_CONFIG)"
		run_timed "Jolt build" clang -c extern/Jolt/jolt_single_file.cpp \
				$GAME_OPT_FLAGS \
				--std=c++20 \
				-I extern \
				-o "$JOLT_LIBRARY" || exit 1
		commit_dependency_manifest jolt
	else
		echo "Using cached Jolt ($GAME_BUILD_CONFIG)"
	fi

	# Main Mac Build
	MAIN_OBJECT="$BUILD_CACHE_DIR/game_main.o"
	echo "Compiling main game ($GAME_BUILD_CONFIG)"
	if ! run_timed "main game compile" clang -c src/main.cpp \
		$GAME_OPT_FLAGS \
		$GAME_WARNING_FLAGS \
		$GAME_FEATURE_FLAGS \
		-o "$MAIN_OBJECT" \
		-I src \
		-I extern \
		-I extern/glfw/include \
		-I extern/imgui \
		-I data \
		-I data/shaders \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		"${VULKAN_INCLUDE_ARGS[@]}" \
		--std=c++20; then
		echo "game compile failed"
		exit 1
	fi

	echo "Linking main game ($GAME_BUILD_CONFIG)"
	if ! run_timed "main game link" clang "$MAIN_OBJECT" \
		-o bin/game \
		-lc++ \
		"$GLFW_LIBRARY" \
		"$VMA_LIBRARY" \
		"$JOLT_LIBRARY" \
		-framework Cocoa \
		-framework IOKit \
		-framework QuartzCore; then
		echo "game link failed"
		exit 1
	fi

	# Point volk's runtime loader at MoltenVK when the caller did not already
	# select a Vulkan driver.
	configure_moltenvk_runtime

	## Run game
	if [ $RUN_GAME -eq 1 ]; then
		./bin/game
	fi
elif [[ $OS_ARG = Windows ]]; then
	# NOTE: drafted from game_old/build.sh's Windows branch + the reference
	# project's build.bat — not yet tested on a Windows machine.
	# Requires: clang + llvm-ar on PATH, VULKAN_SDK env var set by the SDK
	# installer, glslc on PATH.
	rm -rf bin/game.exe

	AR_BIN=$(command -v llvm-ar || command -v ar)

	# Compile GLFW as a static library when its vendored directory changes.
	GLFW_LIBRARY="$BUILD_CACHE_DIR/glfw.lib"
	GLFW_OBJECT_DIR="$BUILD_CACHE_DIR/glfw_obj"
	GLFW_SOURCES="win32_init.c win32_joystick.c win32_module.c win32_monitor.c win32_thread.c win32_time.c win32_window.c wgl_context.c context.c egl_context.c init.c input.c monitor.c null_init.c null_joystick.c null_monitor.c null_window.c osmesa_context.c platform.c vulkan.c window.c"
	if dependency_needs_rebuild glfw extern/glfw "$GLFW_LIBRARY"; then
		echo "Building GLFW ($GAME_BUILD_CONFIG)"
		TIMEFORMAT="[timing] GLFW build: %3R s"
		time {
			rm -rf "$GLFW_OBJECT_DIR"
			mkdir -p "$GLFW_OBJECT_DIR"
			for f in $GLFW_SOURCES; do
				clang -c extern/glfw/src/$f \
						$GAME_OPT_FLAGS \
						-D _GLFW_WIN32 \
						-I extern/glfw/include \
						-o "$GLFW_OBJECT_DIR/${f%.*}.o" || exit 1
			done
			"$AR_BIN" rcs "$GLFW_LIBRARY" "$GLFW_OBJECT_DIR"/*.o || exit 1
		}
		commit_dependency_manifest glfw
	else
		echo "Using cached GLFW ($GAME_BUILD_CONFIG)"
	fi

	# Compile VMA as a static library when its vendored directory changes.
	VMA_LIBRARY="$BUILD_CACHE_DIR/vma.lib"
	if dependency_needs_rebuild vma extern/vma "$VMA_LIBRARY"; then
		echo "Building VMA ($GAME_BUILD_CONFIG)"
		run_timed "VMA build" clang -c extern/vma/vma_single_file.cpp \
				$GAME_OPT_FLAGS \
				--std=c++20 \
				-Wno-everything \
				"${VULKAN_INCLUDE_ARGS[@]}" \
				-o "$VMA_LIBRARY" || exit 1
		commit_dependency_manifest vma
	else
		echo "Using cached VMA ($GAME_BUILD_CONFIG)"
	fi

	# Compile Jolt as a static library when its vendored directory changes.
	# (NOTE: correct extern/Jolt path — game_old/'s Windows branch still
	# points at a stale location. Keep JPH_* defines empty on both compiles.)
	JOLT_LIBRARY="$BUILD_CACHE_DIR/jolt.lib"
	if dependency_needs_rebuild jolt extern/Jolt "$JOLT_LIBRARY"; then
		echo "Building Jolt ($GAME_BUILD_CONFIG)"
		run_timed "Jolt build" clang -c extern/Jolt/jolt_single_file.cpp \
				$GAME_OPT_FLAGS \
				--std=c++20 \
				-I extern \
				-o "$JOLT_LIBRARY" || exit 1
		commit_dependency_manifest jolt
	else
		echo "Using cached Jolt ($GAME_BUILD_CONFIG)"
	fi

	# Main Windows Build
	MAIN_OBJECT="$BUILD_CACHE_DIR/game_main.obj"
	echo "Compiling main game ($GAME_BUILD_CONFIG)"
	if ! run_timed "main game compile" clang -c src/main.cpp \
		$GAME_OPT_FLAGS \
		$GAME_WARNING_FLAGS \
		$GAME_FEATURE_FLAGS \
		-o "$MAIN_OBJECT" \
		-I src \
		-I extern \
		-I extern/glfw/include \
		-I extern/imgui \
		"${VULKAN_INCLUDE_ARGS[@]}" \
		-I data \
		-I data/shaders \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		-D _CRT_SECURE_NO_WARNINGS \
		--std=c++20; then
		echo "game compile failed"
		exit 1
	fi

	echo "Linking main game ($GAME_BUILD_CONFIG)"
	if ! run_timed "main game link" clang "$MAIN_OBJECT" \
		-o bin/game.exe \
		"$GLFW_LIBRARY" \
		"$VMA_LIBRARY" \
		"$JOLT_LIBRARY" \
		-lUser32 \
		-lgdi32 \
		-lshell32; then
		echo "game link failed"
		exit 1
	fi

	## Run game (volk finds vulkan-1.dll from the system loader install)
	if [ $RUN_GAME -eq 1 ]; then
		./bin/game.exe
	fi
elif [[ $OS_ARG = Linux ]]; then
	rm -f bin/game

	# Build GLFW's X11 backend. GLFW loads the optional X11 extension libraries
	# dynamically, so the final binary only needs the standard Linux dl/thread
	# libraries.
	GLFW_LIBRARY="$BUILD_CACHE_DIR/libglfw.a"
	GLFW_OBJECT_DIR="$BUILD_CACHE_DIR/glfw_obj"
	GLFW_SOURCES="x11_init.c x11_monitor.c x11_window.c xkb_unicode.c linux_joystick.c posix_module.c posix_poll.c posix_thread.c posix_time.c context.c egl_context.c glx_context.c init.c input.c monitor.c null_init.c null_joystick.c null_monitor.c null_window.c osmesa_context.c platform.c vulkan.c window.c"
	if dependency_needs_rebuild glfw extern/glfw "$GLFW_LIBRARY"; then
		echo "Building GLFW X11 backend ($GAME_BUILD_CONFIG)"
		TIMEFORMAT="[timing] GLFW build: %3R s"
		time {
			rm -rf "$GLFW_OBJECT_DIR"
			mkdir -p "$GLFW_OBJECT_DIR"
			for f in $GLFW_SOURCES; do
				clang -c "extern/glfw/src/$f" \
						$GAME_OPT_FLAGS \
						-D _GLFW_X11 \
						-I extern/glfw/include \
						-o "$GLFW_OBJECT_DIR/${f%.*}.o" || exit 1
			done
			ar rcs "$GLFW_LIBRARY" "$GLFW_OBJECT_DIR"/*.o || exit 1
		}
		commit_dependency_manifest glfw
	else
		echo "Using cached GLFW ($GAME_BUILD_CONFIG)"
	fi

	VMA_LIBRARY="$BUILD_CACHE_DIR/libvma.a"
	if dependency_needs_rebuild vma extern/vma "$VMA_LIBRARY"; then
		echo "Building VMA ($GAME_BUILD_CONFIG)"
		TIMEFORMAT="[timing] VMA build: %3R s"
		time {
			clang++ -c extern/vma/vma_single_file.cpp \
					$GAME_OPT_FLAGS \
					--std=c++20 \
					-Wno-everything \
					"${VULKAN_INCLUDE_ARGS[@]}" \
					-o "$BUILD_CACHE_DIR/vma.o" || exit 1
			ar rcs "$VMA_LIBRARY" "$BUILD_CACHE_DIR/vma.o" || exit 1
		}
		commit_dependency_manifest vma
	else
		echo "Using cached VMA ($GAME_BUILD_CONFIG)"
	fi

	JOLT_LIBRARY="$BUILD_CACHE_DIR/libjolt.a"
	if dependency_needs_rebuild jolt extern/Jolt "$JOLT_LIBRARY"; then
		echo "Building Jolt ($GAME_BUILD_CONFIG)"
		TIMEFORMAT="[timing] Jolt build: %3R s"
		time {
			clang++ -c extern/Jolt/jolt_single_file.cpp \
					$GAME_OPT_FLAGS \
					--std=c++20 \
					-I extern \
					-o "$BUILD_CACHE_DIR/jolt.o" || exit 1
			ar rcs "$JOLT_LIBRARY" "$BUILD_CACHE_DIR/jolt.o" || exit 1
		}
		commit_dependency_manifest jolt
	else
		echo "Using cached Jolt ($GAME_BUILD_CONFIG)"
	fi

	MAIN_OBJECT="$BUILD_CACHE_DIR/game_main.o"
	echo "Compiling main game ($GAME_BUILD_CONFIG)"
	if ! run_timed "main game compile" clang++ -c src/main.cpp \
		$GAME_OPT_FLAGS \
		$GAME_WARNING_FLAGS \
		$GAME_FEATURE_FLAGS \
		-o "$MAIN_OBJECT" \
		-I src \
		-I extern \
		-I extern/glfw/include \
		-I extern/imgui \
		-I data \
		-I data/shaders \
		-I bin/shaders \
		-I ../flatbuffers/include \
		-I ../compiled_schemas/cpp \
		"${VULKAN_INCLUDE_ARGS[@]}" \
		--std=c++20 \
		-pthread; then
		echo "game compile failed"
		exit 1
	fi

	echo "Linking main game ($GAME_BUILD_CONFIG)"
	if ! run_timed "main game link" clang++ "$MAIN_OBJECT" \
		-o bin/game \
		"$GLFW_LIBRARY" \
		"$VMA_LIBRARY" \
		"$JOLT_LIBRARY" \
		-ldl \
		-pthread; then
		echo "game link failed"
		exit 1
	fi

	if [ $RUN_GAME -eq 1 ]; then
		./bin/game
	fi
else
	echo "Invalid OS Passed to game/build.sh"
	exit 1
fi
