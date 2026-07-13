#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BLEND_SRC_DIR="$SCRIPT_DIR/blend_src"
BLENDER_SRC_DIR="$BLEND_SRC_DIR/blender"

DRY_RUN=0
RESET_SOURCE=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--dry-run)
			DRY_RUN=1
			;;
		--reset-source)
			RESET_SOURCE=1
			;;
		-h|--help)
			echo "Usage: $0 [--dry-run] [--reset-source]"
			echo "  --reset-source  Also remove Blender source so the next build creates a fresh Git checkout."
			exit 0
			;;
		*)
			echo "Error: unknown argument '$1'"
			echo "Usage: $0 [--dry-run] [--reset-source]"
			exit 1
			;;
	esac
	shift
done

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
if [[ $RESET_SOURCE -eq 1 ]]; then
	echo "Resetting $BLENDER_SRC_DIR; the next native build will create a fresh Git checkout."
else
	echo "Preserving $BLENDER_SRC_DIR"
fi
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

if [[ $RESET_SOURCE -eq 1 ]]; then
	targets+=(
		"$BLENDER_SRC_DIR"
		"$BLEND_SRC_DIR/.patches_applied"
	)
fi

for target in "${targets[@]}"; do
	remove_path "$target"
done

shopt -s nullglob
for target in "$BLEND_SRC_DIR"/extract.* "$BLEND_SRC_DIR"/downloads/*.partial; do
	remove_path "$target"
done
shopt -u nullglob

echo "Done."
