#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

usage() {
	echo "Usage: ./screenshots.sh <capture-path>"
	echo "Example: ./screenshots.sh test1"
	echo "Nested example: ./screenshots.sh experiments/step1"
}

if [[ $# -ne 1 || -z "$1" ]]; then
	usage
	exit 1
fi

CAPTURE_PATH=$1
if [[ "$CAPTURE_PATH" = /* ]]; then
	echo "Error: capture path must be relative to the screenshots directory"
	exit 1
fi
if [[ "$CAPTURE_PATH" = */ ]]; then
	echo "Error: capture path cannot end with '/'"
	exit 1
fi

IFS='/' read -r -a CAPTURE_PATH_PARTS <<< "$CAPTURE_PATH"
for path_part in "${CAPTURE_PATH_PARTS[@]}"; do
	if [[ -z "$path_part" || "$path_part" = "." || "$path_part" = ".." ]]; then
		echo "Error: capture path cannot contain empty, '.' or '..' components"
		exit 1
	fi
	if [[ ! "$path_part" =~ ^[A-Za-z0-9._-]+$ ]]; then
		echo "Error: capture path components may contain only letters, numbers, '.', '_', and '-'"
		exit 1
	fi
done

# Keep the deterministic A/B scene matrix here. Each scene gets one PPM named
# after its Blend file inside the selected capture-set directory.
SCENES=(
	"test_file.blend"
	"shadow_test.blend"
)

OUTPUT_DIR="screenshots/$CAPTURE_PATH"

echo "Capturing ${#SCENES[@]} scenes into $OUTPUT_DIR"
for scene in "${SCENES[@]}"; do
	echo "Capturing $scene"
	"$SCRIPT_DIR/build.sh" -native -f "$scene" -screenshot "$OUTPUT_DIR"
done

echo "Screenshot set complete: $SCRIPT_DIR/$OUTPUT_DIR"
