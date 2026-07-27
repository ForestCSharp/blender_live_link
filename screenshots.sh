#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

usage() {
	echo "Usage: ./screenshots.sh <step-name>"
	echo "Example: ./screenshots.sh 00_no_changes"
}

if [[ $# -ne 1 || -z "$1" ]]; then
	usage
	exit 1
fi

STEP_NAME=$1
if [[ ! "$STEP_NAME" =~ ^[A-Za-z0-9._-]+$ ]]; then
	echo "Error: step name may contain only letters, numbers, '.', '_', and '-'"
	exit 1
fi

# Keep the deterministic A/B scene matrix here. Each scene gets one PPM named
# after its Blend file inside the selected capture-set directory.
SCENES=(
	"test_file.blend"
	"shadow_test.blend"
)

OUTPUT_DIR="screenshots/gbuffer_compact/$STEP_NAME"

echo "Capturing ${#SCENES[@]} scenes into $OUTPUT_DIR"
for scene in "${SCENES[@]}"; do
	echo "Capturing $scene"
	"$SCRIPT_DIR/build.sh" -native -f "$scene" -screenshot "$OUTPUT_DIR"
done

echo "Screenshot set complete: $SCRIPT_DIR/$OUTPUT_DIR"
