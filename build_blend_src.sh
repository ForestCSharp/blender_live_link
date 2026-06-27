#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BLEND_SRC_DIR="$SCRIPT_DIR/blend_src"
BLENDER_SRC_DIR="$BLEND_SRC_DIR/blender"
DOWNLOAD_DIR="$BLEND_SRC_DIR/downloads"
TOOLS_DIR="$BLEND_SRC_DIR/tools"
BLENDER_BUILD_DIR="$BLEND_SRC_DIR/build_macos"
BLENDER_LITE_BUILD_DIR="${BLENDER_BUILD_DIR}_lite"
BLENDER_BINARY="$BLENDER_LITE_BUILD_DIR/bin/Blender.app/Contents/MacOS/Blender"
BLENDER_PATCH_DIR="$SCRIPT_DIR/blend_patches"
BLENDER_PATCH_SENTINEL_DIR="$BLEND_SRC_DIR/.patches_applied"
SOURCE_INDEX_URL="${BLENDER_SOURCE_INDEX_URL:-https://download.blender.org/source/}"
BOOTSTRAP_CMAKE_VERSION="${BLENDER_BOOTSTRAP_CMAKE_VERSION:-3.31.6}"
FAST_BUILD_CMAKE_ARGS="-DWITH_GTESTS=OFF -DWITH_COMPILER_ASAN=OFF -DWITH_ASSERT_RELEASE=OFF -DWITH_BUILDINFO=OFF"
RUBBERBAND_CPP_ARGS="-include cstddef"
BLENDER_PATCHES_CHANGED=false

is_valid_blender_source() {
	[[ -f "$BLENDER_SRC_DIR/CMakeLists.txt" && -d "$BLENDER_SRC_DIR/source/creator" ]]
}

require_command() {
	local command_name=$1
	if ! command -v "$command_name" > /dev/null 2>&1; then
		echo "Error: required command '$command_name' was not found"
		exit 1
	fi
}

patch_checksum() {
	local patch_path=$1

	if command -v shasum > /dev/null 2>&1; then
		shasum -a 256 "$patch_path" | awk '{ print $1 }'
	elif command -v sha256sum > /dev/null 2>&1; then
		sha256sum "$patch_path" | awk '{ print $1 }'
	else
		echo "Error: neither shasum nor sha256sum was found; cannot track Blender patch state" >&2
		exit 1
	fi
}

detect_os() {
	local uname_out
	uname_out="$(uname -s)"
	case "$uname_out" in
		Darwin*) echo Mac;;
		Linux*) echo Linux;;
		CYGWIN*|MINGW*|MSYS_NT*) echo Windows;;
		*) echo "UNKNOWN:$uname_out";;
	esac
}

detect_blender_arch() {
	local machine
	machine="$(uname -m)"
	case "$machine" in
		arm64|aarch64) echo arm64;;
		x86_64|amd64) echo x64;;
		*)
			echo "Error: unsupported macOS architecture '$machine'" >&2
			exit 1
			;;
	esac
}

ensure_nprocs() {
	local detected_nprocs
	detected_nprocs=$(
		getconf _NPROCESSORS_ONLN 2> /dev/null ||
			sysctl -n hw.ncpu 2> /dev/null ||
			python3 -c 'import os; print(os.cpu_count() or 1)' 2> /dev/null ||
			echo 1
	)

	if [[ -n "${BLENDER_BUILD_NPROCS:-}" ]]; then
		NPROCS="$BLENDER_BUILD_NPROCS"
	elif [[ -z "${NPROCS:-}" || "$NPROCS" == "1" ]]; then
		NPROCS="$detected_nprocs"
	fi

	export NPROCS
	echo "Using NPROCS=$NPROCS"
}

find_latest_blender_archive() {
	grep -Eo 'blender-[0-9]+\.[0-9]+\.[0-9]+\.tar\.xz' |
		sort -u |
		sed -E 's/^blender-([0-9]+)\.([0-9]+)\.([0-9]+)\.tar\.xz$/\1 \2 \3 &/' |
		sort -k1,1n -k2,2n -k3,3n |
		tail -n 1 |
		sed -E 's/^[0-9]+ [0-9]+ [0-9]+ //'
}

ensure_blender_source() {
	require_command curl
	require_command tar

	if is_valid_blender_source; then
		echo "Blender source already present at $BLENDER_SRC_DIR"
		return
	fi

	if [[ -e "$BLENDER_SRC_DIR" ]]; then
		if [[ -d "$BLENDER_SRC_DIR" && -z "$(find "$BLENDER_SRC_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
			rmdir "$BLENDER_SRC_DIR"
		else
			echo "Error: $BLENDER_SRC_DIR exists but does not look like Blender source"
			echo "Expected CMakeLists.txt and source/creator. Move it aside before retrying."
			exit 1
		fi
	fi

	mkdir -p "$DOWNLOAD_DIR"

	echo "Checking Blender source releases at $SOURCE_INDEX_URL"
	local source_index
	local archive_name
	source_index=$(curl -fsSL "$SOURCE_INDEX_URL")
	archive_name=$(printf "%s" "$source_index" | find_latest_blender_archive || true)

	if [[ -z "$archive_name" ]]; then
		echo "Error: could not determine latest Blender source archive"
		exit 1
	fi

	local archive_url
	local archive_path
	local partial_archive_path
	archive_url="$SOURCE_INDEX_URL$archive_name"
	archive_path="$DOWNLOAD_DIR/$archive_name"
	partial_archive_path="$archive_path.partial"

	if [[ -f "$archive_path" ]]; then
		echo "Using existing Blender source archive $archive_path"
	else
		echo "Downloading $archive_url"
		rm -f "$partial_archive_path"
		curl -fL "$archive_url" -o "$partial_archive_path"
		mv "$partial_archive_path" "$archive_path"
	fi

	local extract_dir
	extract_dir=$(mktemp -d "$BLEND_SRC_DIR/extract.XXXXXX")
	cleanup() {
		rm -rf "$extract_dir"
		rm -f "$partial_archive_path"
	}
	trap cleanup EXIT

	echo "Extracting $archive_name"
	tar -xf "$archive_path" -C "$extract_dir"

	local extracted_root
	extracted_root=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d -print -quit)
	if [[ -z "$extracted_root" ]]; then
		echo "Error: archive did not contain a source directory"
		exit 1
	fi

	if [[ ! -f "$extracted_root/CMakeLists.txt" || ! -d "$extracted_root/source/creator" ]]; then
		echo "Error: extracted archive does not look like Blender source"
		exit 1
	fi

	mv "$extracted_root" "$BLENDER_SRC_DIR"
	echo "Blender source ready at $BLENDER_SRC_DIR"
}

apply_blender_patches() {
	if [[ ! -d "$BLENDER_PATCH_DIR" ]]; then
		echo "No Blender patch directory found at $BLENDER_PATCH_DIR"
		return
	fi

	shopt -s nullglob
	local patches=("$BLENDER_PATCH_DIR"/*.patch)
	shopt -u nullglob

	if (( ${#patches[@]} == 0 )); then
		echo "No Blender patches found in $BLENDER_PATCH_DIR"
		return
	fi

	require_command patch
	mkdir -p "$BLENDER_PATCH_SENTINEL_DIR"

	local patch_path
	for patch_path in "${patches[@]}"; do
		local patch_name
		local patch_sha
		local sentinel_path
		patch_name=$(basename "$patch_path")
		patch_sha=$(patch_checksum "$patch_path")
		sentinel_path="$BLENDER_PATCH_SENTINEL_DIR/$patch_name.sha256"

		if patch --batch --forward --dry-run -p1 -d "$BLENDER_SRC_DIR" < "$patch_path" > /dev/null 2>&1; then
			echo "Applying Blender patch: $patch_name"
			patch --batch --forward -p1 -d "$BLENDER_SRC_DIR" < "$patch_path"
			printf "%s\n" "$patch_sha" > "$sentinel_path"
			BLENDER_PATCHES_CHANGED=true
		elif patch --batch --reverse --dry-run -p1 -d "$BLENDER_SRC_DIR" < "$patch_path" > /dev/null 2>&1; then
			if [[ -f "$sentinel_path" && "$(cat "$sentinel_path")" == "$patch_sha" ]]; then
				echo "Blender patch already applied: $patch_name"
			else
				echo "Blender patch already present; recording patch state: $patch_name"
				printf "%s\n" "$patch_sha" > "$sentinel_path"
			fi
		else
			echo "Error: Blender patch '$patch_name' could not be applied or cleanly detected as already applied"
			echo "Source may be partially patched or the patch may not match this Blender release."
			exit 1
		fi
	done
}

require_mac_build_tools() {
	ensure_compatible_cmake
	require_command cmake
	require_command make
	require_command ninja
	require_command python3
	require_command xcodebuild
	require_command autoconf
	require_command automake
	require_command bison
	require_command dos2unix
	require_command flex
	require_command glibtoolize
	require_command pkg-config
	require_command tclsh
	require_command yasm
}

cmake_major_version() {
	local cmake_bin=$1
	"$cmake_bin" --version | awk 'NR == 1 { split($3, version, "."); print version[1]; exit }'
}

ensure_compatible_cmake() {
	require_command python3

	if command -v cmake > /dev/null 2>&1; then
		local host_cmake
		local host_cmake_major
		host_cmake=$(command -v cmake)
		host_cmake_major=$(cmake_major_version "$host_cmake")

		if [[ "$host_cmake_major" =~ ^[0-9]+$ && "$host_cmake_major" -lt 4 ]]; then
			echo "Using CMake $("$host_cmake" --version | awk 'NR == 1 { print $3; exit }') at $host_cmake"
			return
		fi
	fi

	local cmake_venv="$TOOLS_DIR/cmake-$BOOTSTRAP_CMAKE_VERSION-venv"
	local cmake_bin="$cmake_venv/bin/cmake"

	if [[ ! -x "$cmake_bin" ]]; then
		echo "Host CMake is missing or too new for Blender's dependency build."
		echo "Installing private CMake $BOOTSTRAP_CMAKE_VERSION into $cmake_venv"
		python3 -m venv "$cmake_venv"
		"$cmake_venv/bin/python" -m pip install --upgrade pip
		"$cmake_venv/bin/python" -m pip install "cmake==$BOOTSTRAP_CMAKE_VERSION"
	fi

	local bootstrapped_cmake_major
	bootstrapped_cmake_major=$(cmake_major_version "$cmake_bin")
	if [[ ! "$bootstrapped_cmake_major" =~ ^[0-9]+$ || "$bootstrapped_cmake_major" -ge 4 ]]; then
		echo "Error: private CMake at $cmake_bin is not a compatible CMake 3.x build"
		exit 1
	fi

	export PATH="$cmake_venv/bin:$PATH"
	hash -r 2> /dev/null || true
	echo "Using private CMake $("$cmake_bin" --version | awk 'NR == 1 { print $3; exit }') at $cmake_bin"
}

has_blender_dependencies() {
	local deps_dir=$1
	[[ -d "$deps_dir/python" && -d "$deps_dir/tbb" ]]
}

ensure_mac_libdir_marker() {
	local deps_dir=$1
	local marker="$deps_dir/.git"

	if [[ -e "$marker" ]]; then
		return
	fi

	echo "Creating Blender precompiled-library marker at $marker"
	echo "Generated by build_blend_src.sh after make deps; this is not a Git checkout." > "$marker"
}

ensure_mac_python_site_packages() {
	echo "Ensuring Blender dependency Python has build-time site packages"
	echo "This avoids python-zstandard failing on missing 'packaging' during make deps."
	ensure_nprocs
	make -C "$BLENDER_SRC_DIR" deps DEPS_TARGET=external_python_site_packages
}

apply_rubberband_size_t_workaround() {
	local arch=$1
	local deps_build_dir="$BLEND_SRC_DIR/build_darwin/deps_$arch"
	local rubberband_build_dir="$deps_build_dir/build/rubberband/src/external_rubberband-build"
	local meson_bin="$deps_build_dir/Release/python/bin/meson"

	if [[ ! -d "$rubberband_build_dir" || ! -x "$meson_bin" ]]; then
		echo "Rubber Band workaround not applicable yet; Meson build directory was not found."
		return 1
	fi

	echo "Applying Rubber Band Meson C++ args workaround: $RUBBERBAND_CPP_ARGS"
	echo "This fixes Xcode/libc++ builds where Rubber Band 4.0.0 uses size_t without including <cstddef>."
	"$meson_bin" configure "$rubberband_build_dir" "-Dcpp_args=$RUBBERBAND_CPP_ARGS"
}

make_deps_with_mac_workarounds() {
	local arch=$1

	if make -C "$BLENDER_SRC_DIR" deps; then
		return
	fi

	echo "Blender dependency build failed; checking known macOS dependency workarounds."
	if apply_rubberband_size_t_workaround "$arch"; then
		echo "Retrying Blender dependency build after Rubber Band workaround."
		make -C "$BLENDER_SRC_DIR" deps
		return
	fi

	echo "No known workaround applied for the dependency failure."
	return 1
}

ensure_mac_blender_dependencies() {
	local arch=$1
	local deps_dir="$BLENDER_SRC_DIR/lib/macos_$arch"

	if has_blender_dependencies "$deps_dir"; then
		ensure_mac_libdir_marker "$deps_dir"
		echo "Blender macOS dependencies already present at $deps_dir"
		return
	fi

	echo "Building Blender macOS dependencies into $deps_dir"
	echo "Skipping 'make update' because blend_src/blender comes from an official source tarball, not a Git checkout."
	ensure_mac_python_site_packages
	ensure_nprocs
	make_deps_with_mac_workarounds "$arch"

	if ! has_blender_dependencies "$deps_dir"; then
		echo "Error: Blender dependency build completed but expected sentinels were not found at $deps_dir"
		echo "Expected at least python/ and tbb/."
		exit 1
	fi

	ensure_mac_libdir_marker "$deps_dir"
}

build_mac_blender() {
	local arch
	arch=$(detect_blender_arch)

	require_mac_build_tools
	ensure_mac_blender_dependencies "$arch"

	if [[ "$BLENDER_PATCHES_CHANGED" == "true" ]]; then
		echo "Blender patches changed; running the app build so native changes are compiled."
	elif [[ -x "$BLENDER_BINARY" ]]; then
		echo "Blender already built at $BLENDER_BINARY"
		return
	fi

	local make_targets=(lite ninja)
	if command -v ccache > /dev/null 2>&1; then
		echo "Using ccache for faster repeated Blender builds."
		make_targets+=(ccache)
	else
		echo "Optional speed-up: install ccache with 'brew install ccache' for faster repeated Blender builds."
	fi

	echo "Building Blender for macOS with lite ninja profile"
	echo "Fast build CMake overrides: $FAST_BUILD_CMAKE_ARGS"
	ensure_nprocs
	BUILD_DIR="$BLENDER_BUILD_DIR" BUILD_CMAKE_ARGS="$FAST_BUILD_CMAKE_ARGS" make -C "$BLENDER_SRC_DIR" "${make_targets[@]}"

	if [[ ! -x "$BLENDER_BINARY" ]]; then
		echo "Error: Blender build completed but binary was not found at $BLENDER_BINARY"
		exit 1
	fi

	echo "Blender built at $BLENDER_BINARY"
}

ensure_blender_source
apply_blender_patches

CURRENT_OS=$(detect_os)
case "$CURRENT_OS" in
	Mac)
		build_mac_blender
		;;
	Windows|Linux)
		echo "Warning: native Blender compilation is currently implemented only for Mac."
		echo "Warning: skipping Blender source compilation on $CURRENT_OS."
		;;
	*)
		echo "Warning: native Blender compilation is currently implemented only for Mac."
		echo "Warning: skipping Blender source compilation on unsupported OS $CURRENT_OS."
		;;
esac
