#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
case "$(uname -s)" in
	Linux*) NATIVE_BLENDER_BINARY="$SCRIPT_DIR/blend_src/build_linux_lite/bin/blender" ;;
	Darwin*) NATIVE_BLENDER_BINARY="$SCRIPT_DIR/blend_src/build_macos_lite/bin/Blender.app/Contents/MacOS/Blender" ;;
	*)
		echo "Error: native Blender parity testing is supported only on macOS and Linux."
		exit 1
		;;
esac
NATIVE_BLENDER_USER_DIR="$SCRIPT_DIR/blend_src/blender_user"
PARITY_LOG_DIR="$SCRIPT_DIR/blend_src/parity_logs"

usage() {
	echo "Usage: ./test_live_link_parity.sh -f <blend-file>"
	echo "  <blend-file> may be a filename under blend_files/, a relative path, or an absolute path."
}

resolve_blend_file_arg() {
	local requested_file=$1
	if [[ "$requested_file" = /* ]]; then
		printf "%s\n" "$requested_file"
	elif [[ -f "$SCRIPT_DIR/blend_files/$requested_file" ]]; then
		printf "%s\n" "$SCRIPT_DIR/blend_files/$requested_file"
	else
		printf "%s\n" "$SCRIPT_DIR/$requested_file"
	fi
}

BLEND_FILE_ARG=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		-f|--file)
			if [[ $# -lt 2 ]]; then
				usage
				exit 1
			fi
			BLEND_FILE_ARG=$2
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown option: $1"
			usage
			exit 1
			;;
	esac
done

if [[ -z "$BLEND_FILE_ARG" ]]; then
	usage
	exit 1
fi

BLEND_FILE=$(resolve_blend_file_arg "$BLEND_FILE_ARG")
if [[ ! -f "$BLEND_FILE" ]]; then
	echo "Error: blend file was not found at $BLEND_FILE"
	exit 1
fi

mkdir -p "$PARITY_LOG_DIR"
LOG_PATH="$PARITY_LOG_DIR/parity_$(date +%Y%m%d_%H%M%S).log"

echo "Building/installing native Live Link for $BLEND_FILE"
BLENDER_LIVE_LINK_SKIP_GAME=1 "$SCRIPT_DIR/build.sh" -native -f "$BLEND_FILE"

if [[ ! -x "$NATIVE_BLENDER_BINARY" ]]; then
	echo "Error: native Blender binary was not found at $NATIVE_BLENDER_BINARY"
	exit 1
fi

echo "Running native/Python parity comparison"
echo "Log: $LOG_PATH"

set +e
BLENDER_USER_CONFIG="$NATIVE_BLENDER_USER_DIR/config" \
BLENDER_USER_SCRIPTS="$NATIVE_BLENDER_USER_DIR/scripts" \
BLENDER_USER_DATAFILES="$NATIVE_BLENDER_USER_DIR/datafiles" \
"$NATIVE_BLENDER_BINARY" "$BLEND_FILE" --background --python-expr '
import bpy
import sys
import traceback

def fail(message):
    print(message)
    sys.exit(1)

try:
    if not hasattr(bpy.ops.live_link, "compare_native_python_export"):
        for module_name in ("blender_live_link", "blender_live_link.extension_main"):
            try:
                bpy.ops.preferences.addon_enable(module=module_name)
            except Exception:
                pass

    if not hasattr(bpy.ops.live_link, "compare_native_python_export"):
        fail("Live Link compare operator is not registered")

    result = bpy.ops.live_link.compare_native_python_export()
    print("LIVE_LINK_PARITY_OPERATOR_RESULT", result)
    if "CANCELLED" in result:
        sys.exit(1)
except SystemExit:
    raise
except Exception:
    print(traceback.format_exc())
    sys.exit(1)
' > "$LOG_PATH" 2>&1
COMPARE_STATUS=$?
set -e

cat "$LOG_PATH"
echo "Parity log written to $LOG_PATH"
exit "$COMPARE_STATUS"
