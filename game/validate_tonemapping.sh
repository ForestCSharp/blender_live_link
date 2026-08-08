#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd "$SCRIPT_DIR"

VENV_DIR="$SCRIPT_DIR/bin/tonemapping-validation-venv"
REQUIREMENTS="$SCRIPT_DIR/tools/tonemapping_validation_requirements.txt"

python_is_supported() {
	"$1" -c 'import sys; raise SystemExit(0 if (3, 9) <= sys.version_info[:2] <= (3, 12) else 1)' \
		>/dev/null 2>&1
}

resolve_python() {
	if [[ -n "${GAME2_TONEMAP_VALIDATION_PYTHON:-}" ]]; then
		local requested
		requested=$(command -v "$GAME2_TONEMAP_VALIDATION_PYTHON" 2>/dev/null || true)
		if [[ -z "$requested" ]]; then
			echo "Error: GAME2_TONEMAP_VALIDATION_PYTHON does not resolve to an executable: $GAME2_TONEMAP_VALIDATION_PYTHON" >&2
			exit 1
		fi
		if ! python_is_supported "$requested"; then
			echo "Error: tonemapping validation requires Python 3.9 through 3.12: $requested" >&2
			exit 1
		fi
		printf '%s\n' "$requested"
		return
	fi

	local candidate resolved
	for candidate in python3.12 python3.11 python3.10 python3.9 python3 python; do
		resolved=$(command -v "$candidate" 2>/dev/null || true)
		if [[ -n "$resolved" ]] && python_is_supported "$resolved"; then
			printf '%s\n' "$resolved"
			return
		fi
	done

	echo "Error: tonemapping validation requires Python 3.9 through 3.12." >&2
	echo "Install Python 3.12 or set GAME2_TONEMAP_VALIDATION_PYTHON to a compatible interpreter." >&2
	exit 1
}

resolve_venv_python() {
	if [[ -x "$VENV_DIR/bin/python" ]]; then
		printf '%s\n' "$VENV_DIR/bin/python"
	elif [[ -x "$VENV_DIR/Scripts/python.exe" ]]; then
		printf '%s\n' "$VENV_DIR/Scripts/python.exe"
	fi
}

BASE_PYTHON=$(resolve_python)
VENV_PYTHON=$(resolve_venv_python)
if [[ -d "$VENV_DIR" ]] && { [[ -z "$VENV_PYTHON" ]] || ! python_is_supported "$VENV_PYTHON"; }; then
	echo "Recreating broken or incompatible tonemapping validation environment"
	rm -rf "$VENV_DIR"
	VENV_PYTHON=""
fi

if [[ -z "$VENV_PYTHON" ]]; then
	echo "Creating tonemapping validation environment with $BASE_PYTHON"
	"$BASE_PYTHON" -m venv "$VENV_DIR"
	VENV_PYTHON=$(resolve_venv_python)
	if [[ -z "$VENV_PYTHON" ]]; then
		echo "Error: the virtual environment did not provide a Python executable: $VENV_DIR" >&2
		exit 1
	fi
fi

if ! "$VENV_PYTHON" - <<'PY' >/dev/null 2>&1
import importlib.metadata
import numpy
import PyOpenColorIO

expected = {"numpy": "2.0.2", "opencolorio": "2.5.0"}
raise SystemExit(0 if all(importlib.metadata.version(name) == version
                          for name, version in expected.items()) else 1)
PY
then
	echo "Installing pinned tonemapping validation dependencies"
	"$VENV_PYTHON" -m pip install --disable-pip-version-check -r "$REQUIREMENTS"
fi

if [[ -f "$VENV_DIR/bin/activate" ]]; then
	# shellcheck disable=SC1091
	source "$VENV_DIR/bin/activate"
else
	# shellcheck disable=SC1091
	source "$VENV_DIR/Scripts/activate"
fi

exec "$VENV_PYTHON" tools/validate_tonemapping.py "$@"
