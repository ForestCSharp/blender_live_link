#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BLEND_SRC_DIR="$SCRIPT_DIR/blend_src"
BLENDER_SRC_DIR="$BLEND_SRC_DIR/blender"

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
	DRY_RUN=1
	shift
fi

if [[ $# -ne 0 ]]; then
	echo "Usage: $0 [--dry-run]"
	exit 1
fi

remove_path() {
	local target=$1

	case "$target" in
		"$BLEND_SRC_DIR"/*) ;;
		*)
			echo "Error: refusing to remove path outside blend_src: $target"
			exit 1
			;;
	esac

	if [[ ! -e "$target" ]]; then
		echo "Skipping missing $target"
		return
	fi

	if [[ $DRY_RUN -eq 1 ]]; then
		echo "Would remove $target"
		return
	fi

	echo "Removing $target"
	rm -rf "$target"
}

echo "Cleaning generated Blender native build state."
echo "Preserving $BLENDER_SRC_DIR"
echo "Preserving $BLEND_SRC_DIR/downloads"

targets=(
	"$BLEND_SRC_DIR/build_darwin"
	"$BLEND_SRC_DIR/build_macos"
	"$BLEND_SRC_DIR/build_macos_lite"
	"$BLEND_SRC_DIR/build_linux"
	"$BLEND_SRC_DIR/build_linux_lite"
	"$BLEND_SRC_DIR/tools"
	"$BLENDER_SRC_DIR/lib/macos_arm64"
	"$BLENDER_SRC_DIR/lib/macos_x64"
	"$BLENDER_SRC_DIR/lib/linux_x64"
)

for target in "${targets[@]}"; do
	remove_path "$target"
done

shopt -s nullglob
for target in "$BLEND_SRC_DIR"/extract.* "$BLEND_SRC_DIR"/downloads/*.partial; do
	remove_path "$target"
done
shopt -u nullglob

echo "Done."
