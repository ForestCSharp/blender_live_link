#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
usage() { echo 'Usage: ./build.sh <Mac|Linux|Windows> [-norun] [-full]'; }
[[ $# -ge 1 ]] || { usage; exit 1; }
case "$1" in Mac|Linux|Windows) ;; *) usage; exit 1 ;; esac
shift
RUN_GAME=true
for arg in "$@"; do
    case "$arg" in
        -norun) RUN_GAME=false ;;
        -full) ;; # Static assets: validation is always a full check.
        *) echo "Unknown argument: $arg"; usage; exit 1 ;;
    esac
done
command -v python3 >/dev/null || { echo 'Python 3 is required; no pip packages are needed.'; exit 1; }
python3 -S "$SCRIPT_DIR/bridge.py" --check
if [[ "$RUN_GAME" = true ]]; then
    exec python3 -S -u "$SCRIPT_DIR/bridge.py"
fi
