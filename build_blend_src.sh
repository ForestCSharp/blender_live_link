#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BLEND_SRC_DIR="$SCRIPT_DIR/blend_src"
BLENDER_SRC_DIR="$BLEND_SRC_DIR/blender"
TOOLS_DIR="$BLEND_SRC_DIR/tools"
HOST_KERNEL=$(uname -s)
case "$HOST_KERNEL" in
	Darwin*) BLENDER_BUILD_DIR="$BLEND_SRC_DIR/build_macos" ;;
	Linux*) BLENDER_BUILD_DIR="$BLEND_SRC_DIR/build_linux" ;;
	*) BLENDER_BUILD_DIR="$BLEND_SRC_DIR/build_${HOST_KERNEL,,}" ;;
esac
BLENDER_LITE_BUILD_DIR="${BLENDER_BUILD_DIR}_lite"
if [[ $HOST_KERNEL = Darwin* ]]; then
	BLENDER_BINARY="$BLENDER_LITE_BUILD_DIR/bin/Blender.app/Contents/MacOS/Blender"
else
	BLENDER_BINARY="$BLENDER_LITE_BUILD_DIR/bin/blender"
fi
BLENDER_PATCH_DIR="$SCRIPT_DIR/blend_patches"
BLENDER_NATIVE_SOURCE_DIR="$BLENDER_PATCH_DIR/live_link_native"
BLENDER_NATIVE_DEST_DIR="$BLENDER_SRC_DIR/source/blender/python/intern"
BLENDER_PATCH_SENTINEL_DIR="$BLEND_SRC_DIR/.patches_applied"
BLENDER_LIVE_LINK_SCHEMA_HEADER="$SCRIPT_DIR/compiled_schemas/cpp/blender_live_link_generated.h"
BLENDER_LIVE_LINK_FLATBUFFERS_HEADER="$SCRIPT_DIR/flatbuffers/include/flatbuffers/flatbuffers.h"
SOURCE_INDEX_URL="${BLENDER_SOURCE_INDEX_URL:-https://download.blender.org/source/}"
BLENDER_GIT_URL="${BLENDER_GIT_URL:-https://projects.blender.org/blender/blender.git}"
DEFAULT_BLENDER_SOURCE_VERSION=5.1.2
FAST_BUILD_CMAKE_ARGS="-DWITH_GTESTS=OFF -DWITH_COMPILER_ASAN=OFF -DWITH_ASSERT_RELEASE=OFF -DWITH_BUILDINFO=OFF -DCMAKE_INSTALL_PREFIX=$BLENDER_LITE_BUILD_DIR/bin -DBLENDER_LIVE_LINK_REPO_DIR=$SCRIPT_DIR"
BLENDER_PATCHES_CHANGED=false
BLENDER_SOURCE_MODE=version
BLENDER_SOURCE_VERSION_EXPLICIT=false
REQUESTED_BLENDER_SOURCE_VERSION="$DEFAULT_BLENDER_SOURCE_VERSION"
REQUESTED_BLENDER_ARCHIVE=""

usage() {
	cat <<EOF
Usage: $0 [--latest | --version X.Y.Z]

Ensures Blender source exists under blend_src/blender, applies Live Link patches,
and builds the local native Blender on macOS or Linux.

Options:
  --latest          Use the highest stable Blender version with a tracked patch set.
  --version X.Y.Z  Use a specific Blender release version.
  -h, --help       Show this help text.

Default:
  Uses pinned Blender source version $DEFAULT_BLENDER_SOURCE_VERSION.

Patch compatibility:
  The requested Blender version must have tracked patches under blend_patches/X.Y.Z/.
EOF
}

parse_args() {
	while [[ $# -gt 0 ]]; do
		case "$1" in
			--latest)
				if [[ "$BLENDER_SOURCE_MODE" != "version" || "$BLENDER_SOURCE_VERSION_EXPLICIT" == "true" ]]; then
					echo "Error: --latest and --version are mutually exclusive"
					exit 1
				fi
				BLENDER_SOURCE_MODE=latest
				REQUESTED_BLENDER_SOURCE_VERSION=""
				shift
				;;
			--version)
				if [[ "$BLENDER_SOURCE_MODE" == "latest" ]]; then
					echo "Error: --latest and --version are mutually exclusive"
					exit 1
				fi
				if [[ $# -lt 2 || "$2" == -* ]]; then
					echo "Error: --version requires a value like 5.1.2"
					exit 1
				fi
				if [[ ! "$2" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
					echo "Error: invalid Blender source version '$2'; expected X.Y.Z"
					exit 1
				fi
				BLENDER_SOURCE_MODE=version
				BLENDER_SOURCE_VERSION_EXPLICIT=true
				REQUESTED_BLENDER_SOURCE_VERSION="$2"
				shift 2
				;;
			-h|--help)
				usage
				exit 0
				;;
			*)
				echo "Error: unknown argument '$1'"
				usage
				exit 1
				;;
		esac
	done
}

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

ensure_git_lfs_on_path() {
	require_command git

	if git lfs version > /dev/null 2>&1; then
		return
	fi

	local private_git_lfs="$TOOLS_DIR/git-lfs-package/usr/bin/git-lfs"
	if [[ -x "$private_git_lfs" ]]; then
		export PATH="$(dirname "$private_git_lfs"):$PATH"
		hash -r 2> /dev/null || true
		git lfs version > /dev/null
		return
	fi

	echo "Error: Git LFS is required for Blender source and precompiled libraries."
	echo "Install git-lfs, then retry."
	exit 1
}

file_checksum() {
	local file_path=$1

	if command -v shasum > /dev/null 2>&1; then
		shasum -a 256 "$file_path" | awk '{ print $1 }'
	elif command -v sha256sum > /dev/null 2>&1; then
		sha256sum "$file_path" | awk '{ print $1 }'
	else
		echo "Error: neither shasum nor sha256sum was found; cannot track Blender native patch state" >&2
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

archive_to_version() {
	local archive_name=$1
	printf "%s" "$archive_name" | sed -E 's/^blender-([0-9]+\.[0-9]+\.[0-9]+)\.tar\.xz$/\1/'
}

resolve_requested_blender_source() {
	case "$BLENDER_SOURCE_MODE" in
		version)
			REQUESTED_BLENDER_ARCHIVE="blender-$REQUESTED_BLENDER_SOURCE_VERSION.tar.xz"
			echo "Using pinned/requested Blender source version $REQUESTED_BLENDER_SOURCE_VERSION"
			;;
		latest)
			require_command curl
			echo "Checking Blender source releases at $SOURCE_INDEX_URL"
			local source_index
			source_index=$(curl -fsSL "$SOURCE_INDEX_URL")
			REQUESTED_BLENDER_ARCHIVE=$(printf "%s" "$source_index" | find_latest_blender_archive || true)
			if [[ -z "$REQUESTED_BLENDER_ARCHIVE" ]]; then
				echo "Error: could not determine latest Blender source archive"
				exit 1
			fi
			REQUESTED_BLENDER_SOURCE_VERSION=$(archive_to_version "$REQUESTED_BLENDER_ARCHIVE")
			echo "Using latest Blender source version $REQUESTED_BLENDER_SOURCE_VERSION"
			;;
		*)
			echo "Error: unknown Blender source mode '$BLENDER_SOURCE_MODE'"
			exit 1
			;;
	esac
}

current_blender_source_version() {
	local version_header="$BLENDER_SRC_DIR/source/blender/blenkernel/BKE_blender_version.h"
	if [[ ! -f "$version_header" ]]; then
		return 1
	fi

	local version_code
	local patch_version
	version_code=$(awk '/^#define BLENDER_VERSION / { print $3; exit }' "$version_header")
	patch_version=$(awk '/^#define BLENDER_VERSION_PATCH / { print $3; exit }' "$version_header")

	if [[ ! "$version_code" =~ ^[0-9]+$ || ! "$patch_version" =~ ^[0-9]+$ ]]; then
		return 1
	fi

	local major=$((version_code / 100))
	local minor=$((version_code % 100))
	printf "%s.%s.%s\n" "$major" "$minor" "$patch_version"
}

ensure_existing_source_matches_requested_version() {
	local current_version
	if ! current_version=$(current_blender_source_version); then
		echo "Error: could not determine Blender source version from $BLENDER_SRC_DIR"
		echo "Expected source/blender/blenkernel/BKE_blender_version.h. Move blend_src/blender aside before retrying."
		exit 1
	fi

	if [[ "$current_version" != "$REQUESTED_BLENDER_SOURCE_VERSION" ]]; then
		echo "Error: existing Blender source is version $current_version, but requested $REQUESTED_BLENDER_SOURCE_VERSION"
		echo "Refusing to replace $BLENDER_SRC_DIR automatically."
		echo "Move or remove blend_src/blender and native build outputs under blend_src/ before retrying."
		exit 1
	fi
}

is_nested_blender_git_checkout() {
	local git_root
	local source_root

	git_root=$(git -C "$BLENDER_SRC_DIR" rev-parse --show-toplevel 2> /dev/null) || return 1
	git_root=$(cd "$git_root" && pwd -P) || return 1
	source_root=$(cd "$BLENDER_SRC_DIR" && pwd -P) || return 1

	[[ "$git_root" == "$source_root" && -f "$BLENDER_SRC_DIR/.gitmodules" ]]
}

ensure_git_blender_source() {
	ensure_git_lfs_on_path

	if is_valid_blender_source; then
		ensure_existing_source_matches_requested_version
		if ! is_nested_blender_git_checkout; then
			echo "Error: native builds require an official Blender Git checkout at $BLENDER_SRC_DIR"
			echo "The existing source is archive-based or belongs to a parent Git repository."
			echo "Run ./clean_blend_src.sh --reset-source, then retry."
			exit 1
		fi
		ensure_blender_tag_checkout
		ensure_blender_source_lfs
		echo "Blender Git source already present at $BLENDER_SRC_DIR"
		return
	fi

	if [[ -e "$BLENDER_SRC_DIR" ]]; then
		echo "Error: $BLENDER_SRC_DIR exists but does not look like Blender source"
		echo "Move it aside before retrying."
		exit 1
	fi

	mkdir -p "$BLEND_SRC_DIR"
	local clone_dir
	clone_dir=$(mktemp -d "$BLEND_SRC_DIR/blender.clone.XXXXXX")
	cleanup_blender_clone() {
		rm -rf "$clone_dir"
	}
	trap cleanup_blender_clone EXIT

	echo "Cloning Blender v$REQUESTED_BLENDER_SOURCE_VERSION from $BLENDER_GIT_URL"
	GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch "v$REQUESTED_BLENDER_SOURCE_VERSION" "$BLENDER_GIT_URL" "$clone_dir"
	if [[ ! -f "$clone_dir/CMakeLists.txt" || ! -d "$clone_dir/source/creator" ]]; then
		echo "Error: cloned repository does not look like Blender source"
		exit 1
	fi

	mv "$clone_dir" "$BLENDER_SRC_DIR"
	trap - EXIT
	ensure_existing_source_matches_requested_version
	ensure_blender_tag_checkout
	ensure_blender_source_lfs
	echo "Blender Git source ready at $BLENDER_SRC_DIR"
}

ensure_blender_tag_checkout() {
	local requested_tag="v$REQUESTED_BLENDER_SOURCE_VERSION"
	local head_commit
	local tag_commit

	if ! git -C "$BLENDER_SRC_DIR" rev-parse --verify --quiet "refs/tags/$requested_tag^{commit}" > /dev/null; then
		echo "Fetching Blender release tag $requested_tag for source verification"
		git -C "$BLENDER_SRC_DIR" fetch --depth 1 origin "refs/tags/$requested_tag:refs/tags/$requested_tag"
	fi

	head_commit=$(git -C "$BLENDER_SRC_DIR" rev-parse HEAD)
	tag_commit=$(git -C "$BLENDER_SRC_DIR" rev-parse "refs/tags/$requested_tag^{commit}")
	if [[ "$head_commit" != "$tag_commit" ]]; then
		echo "Error: Blender source HEAD $head_commit is not the requested release tag $requested_tag ($tag_commit)."
		echo "Move $BLENDER_SRC_DIR aside, then retry to create an exact release checkout."
		exit 1
	fi
}

ensure_blender_source_lfs() {
	local startup_blend="$BLENDER_SRC_DIR/release/datafiles/startup.blend"
	local git_dir
	local lfs_sentinel
	local startup_size=0

	git_dir=$(git -C "$BLENDER_SRC_DIR" rev-parse --absolute-git-dir)
	lfs_sentinel="$git_dir/blender_live_link_source_lfs_no_tests_ready"

	if [[ -f "$startup_blend" ]]; then
		startup_size=$(wc -c < "$startup_blend")
	fi
	if [[ -f "$lfs_sentinel" && "$startup_size" -ge 1024 ]]; then
		echo "Required Blender source LFS assets are already present."
		return
	fi

	echo "Downloading required Blender source LFS assets (excluding the test corpus)"
	git -C "$BLENDER_SRC_DIR" lfs pull --exclude="tests/**"

	if [[ ! -f "$startup_blend" ]] || [[ $(wc -c < "$startup_blend") -lt 1024 ]]; then
		echo "Error: Blender source LFS update completed but startup.blend is still incomplete."
		exit 1
	fi
	printf '%s\n' "v$REQUESTED_BLENDER_SOURCE_VERSION" > "$lfs_sentinel"
}

supported_blender_patch_versions() {
	find "$BLENDER_PATCH_DIR" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; 2> /dev/null |
		grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' |
		sort
}

print_supported_blender_patch_versions() {
	local versions
	versions=$(supported_blender_patch_versions || true)
	if [[ -z "$versions" ]]; then
		echo "  - (none)"
	else
		printf "%s\n" "$versions" | sed 's/^/  - /'
	fi
}

apply_blender_patches() {
	local release_patch_dir="$BLENDER_PATCH_DIR/$REQUESTED_BLENDER_SOURCE_VERSION"
	local release_sentinel_dir="$BLENDER_PATCH_SENTINEL_DIR/$REQUESTED_BLENDER_SOURCE_VERSION"

	if [[ ! -d "$release_patch_dir" ]]; then
		echo "Error: no Blender patch set found for Blender $REQUESTED_BLENDER_SOURCE_VERSION"
		echo "Expected release patch directory: $release_patch_dir"
		echo "Supported Blender patch versions:"
		print_supported_blender_patch_versions
		echo "Using --latest or --version requires a matching blend_patches/X.Y.Z/ patch set."
		exit 1
	fi

	shopt -s nullglob
	local patches=("$release_patch_dir"/*.patch)
	shopt -u nullglob

	if (( ${#patches[@]} == 0 )); then
		echo "Error: no Blender patch files found for Blender $REQUESTED_BLENDER_SOURCE_VERSION"
		echo "Expected one or more .patch files in: $release_patch_dir"
		echo "Supported Blender patch versions:"
		print_supported_blender_patch_versions
		echo "Using --latest or --version requires a matching blend_patches/X.Y.Z/ patch set."
		exit 1
	fi

	require_command patch
	mkdir -p "$release_sentinel_dir"

	local patch_path
	for patch_path in "${patches[@]}"; do
		local patch_name
		local patch_sha
		local sentinel_path
		local legacy_sentinel_path
		patch_name=$(basename "$patch_path")
		patch_sha=$(file_checksum "$patch_path")
		sentinel_path="$release_sentinel_dir/$patch_name.sha256"
		legacy_sentinel_path="$BLENDER_PATCH_SENTINEL_DIR/$patch_name.sha256"

		if patch --batch --forward --dry-run -p1 -d "$BLENDER_SRC_DIR" < "$patch_path" > /dev/null 2>&1; then
			echo "Applying Blender $REQUESTED_BLENDER_SOURCE_VERSION patch: $patch_name"
			patch --batch --forward -p1 -d "$BLENDER_SRC_DIR" < "$patch_path"
			printf "%s\n" "$patch_sha" > "$sentinel_path"
			BLENDER_PATCHES_CHANGED=true
		elif [[ -f "$sentinel_path" && "$(cat "$sentinel_path")" == "$patch_sha" ]]; then
			echo "Blender $REQUESTED_BLENDER_SOURCE_VERSION patch already applied: $patch_name"
		elif [[ -f "$legacy_sentinel_path" && "$(cat "$legacy_sentinel_path")" == "$patch_sha" ]]; then
			echo "Blender $REQUESTED_BLENDER_SOURCE_VERSION patch already applied; migrating patch state: $patch_name"
			printf "%s\n" "$patch_sha" > "$sentinel_path"
		elif patch --batch --reverse --dry-run -p1 -d "$BLENDER_SRC_DIR" < "$patch_path" > /dev/null 2>&1; then
			echo "Blender $REQUESTED_BLENDER_SOURCE_VERSION patch already present; recording patch state: $patch_name"
			printf "%s\n" "$patch_sha" > "$sentinel_path"
		else
			echo "Error: Blender patch '$patch_name' could not be applied or cleanly detected as already applied"
			echo "Source may be partially patched or the patch may not match Blender $REQUESTED_BLENDER_SOURCE_VERSION."
			exit 1
		fi
	done
}

sync_native_live_link_sources() {
	local sources=(
		"bpy_live_link.cc"
		"bpy_live_link.hh"
	)

	mkdir -p "$BLENDER_PATCH_SENTINEL_DIR"

	local source_name
	for source_name in "${sources[@]}"; do
		local source_path="$BLENDER_NATIVE_SOURCE_DIR/$source_name"
		local dest_path="$BLENDER_NATIVE_DEST_DIR/$source_name"
		local source_sha
		local dest_sha=""
		local sentinel_path="$BLENDER_PATCH_SENTINEL_DIR/live_link_native_$source_name.sha256"

		if [[ ! -f "$source_path" ]]; then
			echo "Error: required native Live Link source was not found at $source_path"
			exit 1
		fi

		source_sha=$(file_checksum "$source_path")
		if [[ -f "$dest_path" ]]; then
			dest_sha=$(file_checksum "$dest_path")
		fi

		if [[ "$dest_sha" == "$source_sha" ]]; then
			if [[ -f "$sentinel_path" && "$(cat "$sentinel_path")" == "$source_sha" ]]; then
				echo "Native Live Link source already synced: $source_name"
			else
				echo "Native Live Link source already present; recording source state: $source_name"
				printf "%s\n" "$source_sha" > "$sentinel_path"
			fi
			continue
		fi

		echo "Syncing native Live Link source: $source_name"
		cp "$source_path" "$dest_path"
		printf "%s\n" "$source_sha" > "$sentinel_path"
		BLENDER_PATCHES_CHANGED=true
	done
}

remove_retired_bmesh_dependency_patch() {
	local cmake_file="$BLENDER_SRC_DIR/source/blender/python/intern/CMakeLists.txt"
	local retired_sentinel="$BLENDER_PATCH_SENTINEL_DIR/live_link_native_bmesh_dependency.patch.sha256"

	if [[ ! -f "$cmake_file" ]]; then
		return
	fi

	if ! grep -q '^[[:space:]]*PRIVATE bf::bmesh[[:space:]]*$' "$cmake_file"; then
		rm -f "$retired_sentinel"
		return
	fi

	echo "Removing retired Blender BMesh dependency patch from generated source"
	awk '$0 !~ /^[[:space:]]*PRIVATE bf::bmesh[[:space:]]*$/' "$cmake_file" > "$cmake_file.tmp"
	mv "$cmake_file.tmp" "$cmake_file"
	rm -f "$retired_sentinel"
	BLENDER_PATCHES_CHANGED=true
}

ensure_native_schema_inputs() {
	if [[ ! -f "$BLENDER_LIVE_LINK_SCHEMA_HEADER" ]]; then
		echo "Error: generated C++ schema header was not found at $BLENDER_LIVE_LINK_SCHEMA_HEADER"
		echo "Run ./build.sh -native so FlatBuffers schemas are generated before Blender is compiled."
		exit 1
	fi

	if [[ ! -f "$BLENDER_LIVE_LINK_FLATBUFFERS_HEADER" ]]; then
		echo "Error: FlatBuffers C++ headers were not found at $BLENDER_LIVE_LINK_FLATBUFFERS_HEADER"
		echo "Run ./build.sh -native so flatbuffers/build.sh prepares the dependency first."
		exit 1
	fi
}

require_mac_build_tools() {
	require_command cmake
	require_command make
	require_command ninja
	require_command python3
	require_command xcodebuild
	require_command git
	require_command patch
	ensure_git_lfs_on_path
}

has_blender_dependencies() {
	local deps_dir=$1
	[[ -d "$deps_dir/python" && -d "$deps_dir/tbb" ]]
}

ensure_precompiled_blender_dependencies() {
	local library_name=$1
	local platform_label=$2
	local deps_dir="$BLENDER_SRC_DIR/lib/$library_name"

	if has_blender_dependencies "$deps_dir"; then
		echo "Blender $platform_label precompiled libraries already present at $deps_dir"
		return
	fi

	echo "Downloading Blender precompiled $platform_label libraries"
	echo "The Blender source checkout is not updated; only its matching library submodule is configured."
	(
		cd "$BLENDER_SRC_DIR"
		python3 build_files/utils/make_update.py --no-blender
	)

	if ! has_blender_dependencies "$deps_dir"; then
		echo "Error: Blender library update completed but expected libraries were not found at $deps_dir"
		echo "Expected at least python/ and tbb/."
		exit 1
	fi
}

mac_blender_build_complete() {
	local resource_version="${REQUESTED_BLENDER_SOURCE_VERSION%.*}"
	local resource_scripts="$BLENDER_LITE_BUILD_DIR/bin/Blender.app/Contents/Resources/$resource_version/scripts"
	[[ -x "$BLENDER_BINARY" && -d "$resource_scripts" ]]
}

build_mac_blender() {
	local arch
	arch=$(detect_blender_arch)
	if [[ "$arch" != arm64 ]]; then
		echo "Error: native Blender builds on macOS require Apple Silicon (arm64)."
		echo "Blender 5.1 does not provide precompiled macOS x64 libraries; use ./build.sh -python on Intel Macs."
		exit 1
	fi

	require_mac_build_tools
	ensure_precompiled_blender_dependencies macos_arm64 "macOS ARM64"

	if [[ "$BLENDER_PATCHES_CHANGED" == "true" ]]; then
		echo "Blender patches changed; running the app build so native changes are compiled."
	elif [[ -x "$BLENDER_BINARY" && "$BLENDER_LIVE_LINK_SCHEMA_HEADER" -nt "$BLENDER_BINARY" ]]; then
		echo "Generated Live Link C++ schema is newer than Blender; rebuilding native Blender."
	elif mac_blender_build_complete; then
		echo "Blender already built at $BLENDER_BINARY"
		return
	elif [[ -x "$BLENDER_BINARY" ]]; then
		echo "Blender binary exists but its local application resources are incomplete; finishing the build."
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
	if ! mac_blender_build_complete; then
		echo "Error: Blender build completed but local application resources were not installed."
		exit 1
	fi

	echo "Blender built at $BLENDER_BINARY"
}

require_linux_build_tools() {
	require_command cmake
	require_command make
	require_command ninja
	require_command python3
	require_command git
	require_command pkg-config
	require_command patch
	ensure_git_lfs_on_path
}

build_linux_blender() {
	if [[ $(uname -m) != x86_64 ]]; then
		echo "Error: the native Linux build currently supports x86_64 only."
		exit 1
	fi

	require_linux_build_tools
	ensure_precompiled_blender_dependencies linux_x64 "Linux x64"

	if [[ "$BLENDER_PATCHES_CHANGED" == "true" ]]; then
		echo "Blender patches changed; running the app build so native changes are compiled."
	elif [[ -x "$BLENDER_BINARY" && "$BLENDER_LIVE_LINK_SCHEMA_HEADER" -nt "$BLENDER_BINARY" ]]; then
		echo "Generated Live Link C++ schema is newer than Blender; rebuilding native Blender."
	elif [[ -x "$BLENDER_BINARY" ]]; then
		echo "Blender already built at $BLENDER_BINARY"
		return
	fi

	local make_targets=(lite ninja)
	if command -v ccache > /dev/null 2>&1; then
		echo "Using ccache for faster repeated Blender builds."
		make_targets+=(ccache)
	else
		echo "Optional speed-up: install ccache for faster repeated Blender builds."
	fi

	local linux_cmake_args="$FAST_BUILD_CMAKE_ARGS -DWITH_GHOST_WAYLAND=ON -DWITH_GHOST_X11=ON"
	echo "Building Blender for Linux with lite ninja profile (Wayland + X11)"
	echo "Fast build CMake overrides: $linux_cmake_args"
	ensure_nprocs
	BUILD_DIR="$BLENDER_BUILD_DIR" BUILD_CMAKE_ARGS="$linux_cmake_args" make -C "$BLENDER_SRC_DIR" "${make_targets[@]}"

	if [[ ! -x "$BLENDER_BINARY" ]]; then
		echo "Error: Blender build completed but binary was not found at $BLENDER_BINARY"
		exit 1
	fi

	echo "Blender built at $BLENDER_BINARY"
}

parse_args "$@"
resolve_requested_blender_source
CURRENT_OS=$(detect_os)
case "$CURRENT_OS" in
	Mac)
		if [[ $(detect_blender_arch) != arm64 ]]; then
			echo "Error: native Blender builds on macOS require Apple Silicon (arm64)."
			echo "Blender 5.1 does not provide precompiled macOS x64 libraries; use ./build.sh -python on Intel Macs."
			exit 1
		fi
		;;
	Linux)
		if [[ $(uname -m) != x86_64 ]]; then
			echo "Error: the native Linux build currently supports x86_64 only."
			exit 1
		fi
		;;
	*)
		echo "Error: native Blender compilation is supported only on macOS ARM64 and Linux x64."
		exit 1
		;;
esac
ensure_git_blender_source
ensure_native_schema_inputs
apply_blender_patches
remove_retired_bmesh_dependency_patch
sync_native_live_link_sources

case "$CURRENT_OS" in
	Mac)
		build_mac_blender
		;;
	Linux)
		build_linux_blender
		;;
	Windows)
		echo "Warning: native Blender compilation is currently implemented only for macOS and Linux."
		echo "Warning: skipping Blender source compilation on $CURRENT_OS."
		;;
	*)
		echo "Warning: native Blender compilation is currently implemented only for macOS and Linux."
		echo "Warning: skipping Blender source compilation on unsupported OS $CURRENT_OS."
		;;
esac
