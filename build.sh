#!/usr/bin/env bash

# Running ./build.sh builds native blender integration by default, then builds and runs game in parallel after schema generation
# Running ./build.sh -python builds blender add-on, installs it to blender, and launches blender in parallel with the game after schema generation
# Running ./build.sh -g only rebuilds the default Vulkan game and runs it
# Running ./build.sh --package-only generates schemas and packages the extension without launching either application
# Passing -game_old selects the legacy Sokol runtime (game_old/)

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BASE_DIR="${SCRIPT_DIR##*/}"

# Determine operating system 
unameOut="$(uname -s)"
case "${unameOut}" in
	Linux*)     OS=Linux;;
	Darwin*)    OS=Mac;;
	CYGWIN*)    OS=Windows;;
	MINGW*)     OS=Windows;;
	MSYS_NT*)   OS=Windows;;
	*)          OS="UNKNOWN:${unameOut}"
esac
echo OS is ${OS}

BUILD_ONLY_GAME=false
PACKAGE_ONLY=false
GAME_DIR=game
GAME_BRANCH_LABEL="Game (Vulkan)"
BLENDER_BUILD_MODE=native
BLENDER_BUILD_MODE_WAS_SET=false
if [[ $OS = Linux ]]; then
	NATIVE_BLENDER_BINARY="$SCRIPT_DIR/blend_src/build_linux_lite/bin/blender"
else
	NATIVE_BLENDER_BINARY="$SCRIPT_DIR/blend_src/build_macos_lite/bin/Blender.app/Contents/MacOS/Blender"
fi
NATIVE_BLENDER_USER_DIR="$SCRIPT_DIR/blend_src/blender_user"
EXTENSION_ZIP_PATH="$SCRIPT_DIR/blend_src/$BASE_DIR.zip"
PARALLEL_STATUS_DIR=""
PARALLEL_LOG_DIR=""
PARALLEL_BRANCH_NAMES=()
PARALLEL_BRANCH_PIDS=()
PARALLEL_BRANCH_STATUS_FILES=()
PARALLEL_BRANCH_STATUSES=()
PARALLEL_BRANCH_LOG_FILES=()
PARALLEL_BRANCH_RENDERED_LINES=()
INSTALLED_BLENDER_KIND=""
INSTALLED_BLENDER_COMMAND=()

run_args="$SCRIPT_DIR/blend_files/test_file.blend"

timestamp() {
	date +"%H:%M:%S"
}

log_build() {
	echo "[$(timestamp)] $*"
}

set_blender_build_mode() {
	local requested_mode=$1
	if [[ $BLENDER_BUILD_MODE_WAS_SET = true && $BLENDER_BUILD_MODE != "$requested_mode" ]]; then
		echo "Error: -python and -native are mutually exclusive"
		exit 1
	fi

	BLENDER_BUILD_MODE=$requested_mode
	BLENDER_BUILD_MODE_WAS_SET=true
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

package_extension() {
	log_build "Blender branch: packaging extension"
	mkdir -p "$(dirname "$EXTENSION_ZIP_PATH")" || return
	cd "$SCRIPT_DIR/.." || return
	rm -f "$EXTENSION_ZIP_PATH" || return

	if [[ $OS = Windows ]]; then
		if ! command -v 7z > /dev/null 2>&1; then
			echo "Error: required packaging command '7z' was not found on PATH."
			return 1
		fi
		7z a -tzip "$EXTENSION_ZIP_PATH" $BASE_DIR -w $BASE_DIR/ -r \
			-x!"$BASE_DIR/flatbuffers/*" \
			-x!"$BASE_DIR/.git/*" \
			-x!"$BASE_DIR/.agents/*" \
			-x!"$BASE_DIR/.codex/*" \
			-x!"$BASE_DIR/.claude/*" \
			-x!"$BASE_DIR/.github/*" \
			-x!"$BASE_DIR/tools/*" \
			-x!"$BASE_DIR/ci-artifacts/*" \
			-x!"$BASE_DIR/game/*" \
			-x!"$BASE_DIR/game_old/*" \
			-x!"$BASE_DIR/blend_files/*" \
			-x!"$BASE_DIR/blend_src/*" \
			-x!"$BASE_DIR/blend_patches/*" \
			-x!"$BASE_DIR/compiled_schemas/cpp/*" \
			-x!"$BASE_DIR/docs/*" \
			-x!"$BASE_DIR/__pycache__/*" \
			-x!"*/__pycache__/*" \
			-x!"$BASE_DIR/*.pyc" \
			-x!"$BASE_DIR/build.sh" \
			-x!"$BASE_DIR/build_blend_src.sh" \
			-x!"$BASE_DIR/clean_blend_src.sh" \
			-x!"$BASE_DIR/test_live_link_parity.sh" \
			-x!"$BASE_DIR/README.md" \
			-x!"$BASE_DIR/TODO.txt" \
			-x!"$BASE_DIR/.gitignore" \
			-x!"$BASE_DIR/blender_live_link.fbs" \
			-x!"$BASE_DIR/.DS_Store" \
			-x!"*/.DS_Store" || return
	elif command -v zip > /dev/null 2>&1; then
		zip -r "$EXTENSION_ZIP_PATH" $BASE_DIR \
			-x "$BASE_DIR/flatbuffers/*" \
			-x "$BASE_DIR/.git/*" \
			-x "$BASE_DIR/.agents/*" \
			-x "$BASE_DIR/.codex/*" \
			-x "$BASE_DIR/.claude/*" \
			-x "$BASE_DIR/.github/*" \
			-x "$BASE_DIR/tools/*" \
			-x "$BASE_DIR/ci-artifacts/*" \
			-x "$BASE_DIR/game/*"\
			-x "$BASE_DIR/game_old/*"\
			-x "$BASE_DIR/blend_files/*" \
			-x "$BASE_DIR/blend_src/*" \
			-x "$BASE_DIR/blend_patches/*" \
			-x "$BASE_DIR/compiled_schemas/cpp/*" \
			-x "$BASE_DIR/docs/*" \
			-x "$BASE_DIR/__pycache__/*" \
			-x "*/__pycache__/*" \
			-x "$BASE_DIR/*.pyc" \
			-x "$BASE_DIR/build.sh" \
			-x "$BASE_DIR/build_blend_src.sh" \
			-x "$BASE_DIR/clean_blend_src.sh" \
			-x "$BASE_DIR/test_live_link_parity.sh" \
			-x "$BASE_DIR/README.md" \
			-x "$BASE_DIR/TODO.txt" \
			-x "$BASE_DIR/.gitignore" \
			-x "$BASE_DIR/blender_live_link.fbs" \
			-x "$BASE_DIR/.DS_Store" \
			-x "*/.DS_Store" || return
	elif command -v bsdtar > /dev/null 2>&1; then
		bsdtar -a -cf "$EXTENSION_ZIP_PATH" \
			--exclude "$BASE_DIR/flatbuffers/*" \
			--exclude "$BASE_DIR/.git/*" \
			--exclude "$BASE_DIR/.agents/*" \
			--exclude "$BASE_DIR/.codex/*" \
			--exclude "$BASE_DIR/.claude/*" \
			--exclude "$BASE_DIR/.github/*" \
			--exclude "$BASE_DIR/tools/*" \
			--exclude "$BASE_DIR/ci-artifacts/*" \
			--exclude "$BASE_DIR/game/*" \
			--exclude "$BASE_DIR/game_old/*" \
			--exclude "$BASE_DIR/blend_files/*" \
			--exclude "$BASE_DIR/blend_src/*" \
			--exclude "$BASE_DIR/blend_patches/*" \
			--exclude "$BASE_DIR/compiled_schemas/cpp/*" \
			--exclude "$BASE_DIR/docs/*" \
			--exclude "$BASE_DIR/__pycache__/*" \
			--exclude "*/__pycache__/*" \
			--exclude "$BASE_DIR/*.pyc" \
			--exclude "$BASE_DIR/build.sh" \
			--exclude "$BASE_DIR/build_blend_src.sh" \
			--exclude "$BASE_DIR/clean_blend_src.sh" \
			--exclude "$BASE_DIR/test_live_link_parity.sh" \
			--exclude "$BASE_DIR/README.md" \
			--exclude "$BASE_DIR/TODO.txt" \
			--exclude "$BASE_DIR/.gitignore" \
			--exclude "$BASE_DIR/blender_live_link.fbs" \
			--exclude "$BASE_DIR/.DS_Store" \
			--exclude "*/.DS_Store" \
			"$BASE_DIR" || return
	else
		echo "Error: extension packaging requires either 'zip' or 'bsdtar' on $OS."
		return 1
	fi

	cd "$SCRIPT_DIR" || return
	log_build "Blender branch: packaged extension at $EXTENSION_ZIP_PATH"
}

resolve_installed_blender() {
	INSTALLED_BLENDER_KIND=""
	INSTALLED_BLENDER_COMMAND=()

	if [[ -n "${BLENDER_LIVE_LINK_BLENDER_BINARY:-}" ]]; then
		if [[ ! -x "$BLENDER_LIVE_LINK_BLENDER_BINARY" ]]; then
			echo "Error: BLENDER_LIVE_LINK_BLENDER_BINARY is not executable: $BLENDER_LIVE_LINK_BLENDER_BINARY"
			return 1
		fi
		INSTALLED_BLENDER_KIND=binary
		INSTALLED_BLENDER_COMMAND=("$BLENDER_LIVE_LINK_BLENDER_BINARY")
	elif command -v blender > /dev/null 2>&1; then
		INSTALLED_BLENDER_KIND=binary
		INSTALLED_BLENDER_COMMAND=("$(command -v blender)")
	elif [[ $OS = Linux ]] && command -v flatpak > /dev/null 2>&1 && flatpak info org.blender.Blender > /dev/null 2>&1; then
		INSTALLED_BLENDER_KIND=flatpak
		INSTALLED_BLENDER_COMMAND=(flatpak run org.blender.Blender)
	else
		echo "Error: Blender was not found."
		echo "Install 'blender' on PATH, install Flatpak org.blender.Blender, or set BLENDER_LIVE_LINK_BLENDER_BINARY."
		return 1
	fi
}

install_and_launch_installed_blender() {
	local install_args=(--command extension install-file "$EXTENSION_ZIP_PATH" --repo user_default --enable)

	if [[ $OS = Windows ]]; then
		log_build "Blender branch: installing extension into installed Blender"
		# Note: blender.exe should be on system path on windows
		# kill previous blender instances
		taskkill.exe //F //IM blender.exe || true
		sleep 0.5
		# install add-on and wait for completion
		blender.exe "${install_args[@]}" || return
		sleep 0.5
		# open blender to specified map file
		log_build "Blender branch: launching installed Blender"
		start "" blender.exe $run_args || return
	elif [[ $OS = Mac ]]; then
		log_build "Blender branch: installing extension into /Applications/Blender.app"
		# kill previous blender instances
		killall Blender || true
		# install add-on and wait for completion
		/Applications/Blender.app/Contents/MacOS/Blender "${install_args[@]}" || return
		sleep 0.5
		# open blender without waiting for completion
		log_build "Blender branch: launching /Applications/Blender.app"
		open  /Applications/Blender.app --args $run_args || return
	elif [[ $OS = Linux ]]; then
		resolve_installed_blender || return

		log_build "Blender branch: installing extension into installed Blender"
		if [[ $INSTALLED_BLENDER_KIND = binary ]]; then
			pkill -x blender 2> /dev/null || true
		elif [[ $INSTALLED_BLENDER_KIND = flatpak ]]; then
			flatpak kill org.blender.Blender 2> /dev/null || true
		fi
		"${INSTALLED_BLENDER_COMMAND[@]}" "${install_args[@]}" || return
		sleep 0.5
		log_build "Blender branch: launching installed Blender"
		nohup "${INSTALLED_BLENDER_COMMAND[@]}" "$run_args" > /dev/null 2>&1 &
	else
		echo "Error: installed Blender launch is unsupported on $OS."
		return 1
	fi
}

with_native_blender_profile() {
	mkdir -p "$NATIVE_BLENDER_USER_DIR/config" "$NATIVE_BLENDER_USER_DIR/scripts" "$NATIVE_BLENDER_USER_DIR/datafiles" || return
	BLENDER_USER_CONFIG="$NATIVE_BLENDER_USER_DIR/config" \
	BLENDER_USER_SCRIPTS="$NATIVE_BLENDER_USER_DIR/scripts" \
	BLENDER_USER_DATAFILES="$NATIVE_BLENDER_USER_DIR/datafiles" \
	"$@"
}

install_and_launch_native_blender() {
	if [[ $OS != Mac && $OS != Linux ]]; then
		echo "Error: native Blender launch is unsupported on $OS."
		return 1
	fi

	if [[ ! -x "$NATIVE_BLENDER_BINARY" ]]; then
		echo "Error: native Blender binary was not found at $NATIVE_BLENDER_BINARY"
		echo "Expected build_blend_src.sh to build it before extension install/launch."
		exit 1
	fi

	local install_args=(--command extension install-file "$EXTENSION_ZIP_PATH" --repo user_default --enable)

	if [[ $OS = Linux ]]; then
		pkill -x blender 2> /dev/null || true
	fi

	echo "Installing extension into local Blender profile at $NATIVE_BLENDER_USER_DIR"
	with_native_blender_profile "$NATIVE_BLENDER_BINARY" "${install_args[@]}" || return
	sleep 0.5

	echo "Launching local Blender at $NATIVE_BLENDER_BINARY"
	with_native_blender_profile "$NATIVE_BLENDER_BINARY" "$run_args" &
}

prepare_flatbuffers_and_schemas() {
	# build flatbuffers, passing in OS as first arg
	cd "$SCRIPT_DIR" || return
	local previous_schema_dir
	local previous_schema_header
	local had_previous_schema=false
	previous_schema_dir=$(mktemp -d "${TMPDIR:-/tmp}/blender_live_link_schema.XXXXXX") || return
	previous_schema_header="$previous_schema_dir/blender_live_link_generated.h"
	if [[ -f compiled_schemas/cpp/blender_live_link_generated.h ]]; then
		cp -p compiled_schemas/cpp/blender_live_link_generated.h "$previous_schema_header" || {
			rm -rf "$previous_schema_dir"
			return 1
		}
		had_previous_schema=true
	fi

	local generation_status=0
	(
		rm -rf compiled_schemas || exit
		mkdir -p compiled_schemas/cpp || exit
		./flatbuffers/build.sh "$OS" || exit

		FLATC_BINARY=""
		if [[ $OS = Windows ]]; then
			for candidate in \
				flatbuffers/build/Release/flatc.exe \
				flatbuffers/build/flatc.exe \
				flatbuffers/build/Debug/flatc.exe; do
				if [[ -f "$candidate" ]]; then
					FLATC_BINARY="$candidate"
					break
				fi
			done
		elif [[ -f flatbuffers/build/flatc ]]; then
			FLATC_BINARY=flatbuffers/build/flatc
		fi

		if [[ -z "$FLATC_BINARY" ]]; then
			echo "Error: the FlatBuffers build completed without producing flatc for $OS." >&2
			echo "Checked beneath: $SCRIPT_DIR/flatbuffers/build" >&2
			exit 1
		fi

		# compile schema for cpp
		touch compiled_schemas/__init__.py || exit
		echo "FLATC IS $FLATC_BINARY"
		"$FLATC_BINARY" -o compiled_schemas/cpp --cpp blender_live_link.fbs || exit

		mkdir -p compiled_schemas/python || exit
		touch compiled_schemas/python/__init__.py || exit
		"$FLATC_BINARY" -o compiled_schemas/python --python blender_live_link.fbs || exit

		# copy flatbuffers python package
		cp -a flatbuffers/python/flatbuffers/. compiled_schemas/python/flatbuffers || exit
		rm -rf compiled_schemas/python/flatbuffers/reflection || exit
		rewrite_python_schema_imports || exit
	) || generation_status=$?

	if [[ $generation_status -ne 0 ]]; then
		rm -rf "$previous_schema_dir"
		return "$generation_status"
	fi

	if [[ "$had_previous_schema" == true ]] && cmp -s "$previous_schema_header" compiled_schemas/cpp/blender_live_link_generated.h; then
		cp -p "$previous_schema_header" compiled_schemas/cpp/blender_live_link_generated.h || {
			rm -rf "$previous_schema_dir"
			return 1
		}
		echo "Generated C++ schema is unchanged; preserved its timestamp."
	fi
	rm -rf "$previous_schema_dir"
}

rewrite_python_schema_imports() {
	local schema_dir="compiled_schemas/python/Blender/LiveLink"
	if [[ ! -d "$schema_dir" ]]; then
		echo "Error: expected generated Python schemas at $schema_dir"
		return 1
	fi

	perl -0pi -e '
		s/^import flatbuffers$/from ... import flatbuffers/mg;
		s/^from flatbuffers\.compat import import_numpy$/from ...flatbuffers.compat import import_numpy/mg;
		s/^(\s*)from flatbuffers\.table import Table$/${1}from ...flatbuffers.table import Table/mg;
		s/^(\s*)from Blender\.LiveLink\.([A-Za-z_][A-Za-z0-9_]*) import ([A-Za-z_][A-Za-z0-9_]*)$/${1}from .${2} import ${3}/mg;
	' "$schema_dir"/*.py || return
}

run_blender_side_build_and_launch() {
	cd "$SCRIPT_DIR" || return
	log_build "Blender branch: starting $BLENDER_BUILD_MODE mode"

	if [[ $BLENDER_BUILD_MODE = native ]]; then
		log_build "Blender branch: ensuring local Blender source/build"
		"$SCRIPT_DIR/build_blend_src.sh" || return
	fi

	package_extension || return

	if [[ $BLENDER_BUILD_MODE = python ]]; then
		log_build "Blender branch: installing and launching installed Blender"
		install_and_launch_installed_blender
	else
		log_build "Blender branch: installing and launching native Blender"
		install_and_launch_native_blender
	fi
}

run_game_build_and_launch() {
	cd "$SCRIPT_DIR/$GAME_DIR" || return
	log_build "$GAME_BRANCH_LABEL branch: building and launching $GAME_DIR"
	./build.sh "$OS"
}

cleanup_parallel_status_dir() {
	if [[ -n "$PARALLEL_STATUS_DIR" && -d "$PARALLEL_STATUS_DIR" ]]; then
		rm -rf "$PARALLEL_STATUS_DIR"
	fi
	PARALLEL_STATUS_DIR=""
}

initialize_parallel_dirs() {
	if [[ -z "$PARALLEL_STATUS_DIR" ]]; then
		PARALLEL_STATUS_DIR=$(mktemp -d "${TMPDIR:-/tmp}/blender_live_link_build.XXXXXX") || return
	fi

	if [[ -z "$PARALLEL_LOG_DIR" ]]; then
		local log_timestamp
		log_timestamp=$(date +"%Y%m%d_%H%M%S")
		PARALLEL_LOG_DIR="$SCRIPT_DIR/blend_src/build_logs/$log_timestamp"
		if [[ -e "$PARALLEL_LOG_DIR" ]]; then
			PARALLEL_LOG_DIR="$SCRIPT_DIR/blend_src/build_logs/${log_timestamp}_$$"
		fi
		mkdir -p "$PARALLEL_LOG_DIR" || return
	fi
}

parallel_log_name_for_branch() {
	printf "%s" "$1" | tr '[:upper:]' '[:lower:]' | tr -cs '[:alnum:]' '_'
}

parallel_renderer_enabled() {
	[[ "${BLENDER_LIVE_LINK_SIDE_BY_SIDE:-1}" != "0" ]] || return 1
	[[ -t 1 ]] || return 1
	[[ -n "${TERM:-}" ]] || return 1
}

parallel_branch_status_label() {
	local branch_index=$1
	local branch_status=${PARALLEL_BRANCH_STATUSES[$branch_index]}
	local status_file="${PARALLEL_BRANCH_STATUS_FILES[$branch_index]}"

	if [[ $branch_status -eq -1 ]]; then
		if [[ -f "$status_file" ]]; then
			printf "finishing"
		else
			printf "running"
		fi
	elif [[ $branch_status -eq 0 ]]; then
		printf "success"
	else
		printf "failed:%s" "$branch_status"
	fi
}

print_two_column_line() {
	local left=$1
	local right=$2
	local pane_width=$3

	printf "%-*s | %-*s\n" "$pane_width" "$left" "$pane_width" "$right"
}

parallel_terminal_cols() {
	local cols
	cols="${BLENDER_LIVE_LINK_RENDER_COLS:-}"
	if [[ -z "$cols" ]]; then
		local stty_size
		stty_size=$(stty size 2> /dev/null || printf "")
		if [[ "$stty_size" =~ ^[0-9]+[[:space:]]+([0-9]+)$ ]]; then
			cols="${BASH_REMATCH[1]}"
		fi
	fi
	if [[ -z "$cols" ]]; then
		cols=$(tput cols 2> /dev/null || printf "")
	fi
	if [[ -z "$cols" || ! "$cols" =~ ^[0-9]+$ ]]; then
		cols=120
	fi
	if (( cols < 60 )); then
		cols=60
	fi
	printf "%s\n" "$cols"
}

parallel_pane_width() {
	local cols
	cols=$(parallel_terminal_cols)
	printf "%s\n" "$(((cols - 3) / 2))"
}

format_parallel_log_lines() {
	local pane_width=$1
	local wrap_lines="${BLENDER_LIVE_LINK_WRAP_LINES:-1}"
	awk -v width="$pane_width" -v wrap_lines="$wrap_lines" '
	{
		gsub(/\r/, "")
		gsub(/\t/, "  ")
		if (wrap_lines == "0") {
			if (length($0) > width) {
				print substr($0, 1, width - 1) ">"
			}
			else {
				print $0
			}
		}
		else {
			line = $0
			if (line == "") {
				print ""
			}
			while (length(line) > width) {
				print substr(line, 1, width)
				line = substr(line, width + 1)
			}
			if (line != "") {
				print line
			}
		}
	}'
}

print_parallel_render_header() {
	local pane_width
	pane_width=$(parallel_pane_width)

	local left_index=0
	local right_index=1
	local left_header="${PARALLEL_BRANCH_NAMES[$left_index]} pid ${PARALLEL_BRANCH_PIDS[$left_index]}"
	local right_header="${PARALLEL_BRANCH_NAMES[$right_index]} pid ${PARALLEL_BRANCH_PIDS[$right_index]}"
	local cols
	cols=$(parallel_terminal_cols)

	print_two_column_line "$left_header" "$right_header" "$pane_width"
	printf "%s\n" "$(printf "%*s" "$cols" "" | tr ' ' '-')"
}

write_new_parallel_lines_for_branch() {
	local branch_index=$1
	local pane_width
	pane_width=$(parallel_pane_width)
	local log_file="${PARALLEL_BRANCH_LOG_FILES[$branch_index]}"
	local rendered_lines=${PARALLEL_BRANCH_RENDERED_LINES[$branch_index]}
	local current_lines

	current_lines=$(wc -l < "$log_file" 2> /dev/null || printf "0")
	current_lines=${current_lines//[[:space:]]/}
	if [[ -z "$current_lines" || ! "$current_lines" =~ ^[0-9]+$ ]]; then
		current_lines=0
	fi

	if (( current_lines <= rendered_lines )); then
		: > "$PARALLEL_STATUS_DIR/render_${branch_index}.new"
		return
	fi

	sed -n "$((rendered_lines + 1)),${current_lines}p" "$log_file" | format_parallel_log_lines "$pane_width" > "$PARALLEL_STATUS_DIR/render_${branch_index}.new"
	PARALLEL_BRANCH_RENDERED_LINES[$branch_index]=$current_lines
}

render_parallel_logs() {
	local branch_count=${#PARALLEL_BRANCH_PIDS[@]}
	if (( branch_count < 2 )); then
		return
	fi

	local pane_width
	pane_width=$(parallel_pane_width)

	write_new_parallel_lines_for_branch 0
	write_new_parallel_lines_for_branch 1

	paste "$PARALLEL_STATUS_DIR/render_0.new" "$PARALLEL_STATUS_DIR/render_1.new" | awk -v left_w="$pane_width" -v right_w="$pane_width" -F '\t' '
	{
		printf "%-*s | %-*s\n", left_w, $1, right_w, $2
	}'
}

print_parallel_status_summary() {
	local pane_width
	pane_width=$(parallel_pane_width)
	local left_status
	local right_status
	left_status=$(parallel_branch_status_label 0)
	right_status=$(parallel_branch_status_label 1)

	print_two_column_line "${PARALLEL_BRANCH_NAMES[0]} [$left_status]" "${PARALLEL_BRANCH_NAMES[1]} [$right_status]" "$pane_width"
}

print_parallel_log_locations() {
	if [[ -n "$PARALLEL_LOG_DIR" ]]; then
		echo "Parallel build logs: $PARALLEL_LOG_DIR"
	fi
}

print_parallel_log_tails() {
	local branch_index
	print_parallel_log_locations
	for (( branch_index = 0; branch_index < ${#PARALLEL_BRANCH_LOG_FILES[@]}; branch_index++ )); do
		echo "--- ${PARALLEL_BRANCH_NAMES[$branch_index]} log tail (${PARALLEL_BRANCH_LOG_FILES[$branch_index]}) ---"
		tail -n 40 "${PARALLEL_BRANCH_LOG_FILES[$branch_index]}" 2> /dev/null || true
	done
}

terminate_process_tree() {
	local pid=$1

	if ! kill -0 "$pid" 2> /dev/null; then
		return
	fi

	if command -v pgrep > /dev/null 2>&1; then
		local child_pid
		for child_pid in $(pgrep -P "$pid" 2> /dev/null); do
			terminate_process_tree "$child_pid"
		done
	fi

	kill "$pid" 2> /dev/null || true
}

terminate_active_parallel_branches() {
	local failed_index=$1
	local branch_index

	for (( branch_index = 0; branch_index < ${#PARALLEL_BRANCH_PIDS[@]}; branch_index++ )); do
		if [[ $branch_index -eq $failed_index || ${PARALLEL_BRANCH_STATUSES[$branch_index]} -ne -1 ]]; then
			continue
		fi

		echo "Stopping ${PARALLEL_BRANCH_NAMES[$branch_index]} branch after ${PARALLEL_BRANCH_NAMES[$failed_index]} branch failed"
		terminate_process_tree "${PARALLEL_BRANCH_PIDS[$branch_index]}"
	done
}

start_parallel_branch() {
	local branch_name=$1
	shift

	initialize_parallel_dirs || return

	local branch_index=${#PARALLEL_BRANCH_PIDS[@]}
	local status_file="$PARALLEL_STATUS_DIR/$branch_index.status"
	local log_name
	log_name=$(parallel_log_name_for_branch "$branch_name")
	local log_file="$PARALLEL_LOG_DIR/$log_name.log"
	: > "$log_file" || return

	(
		{
			log_build "$branch_name branch started"
			"$@"
			local branch_status=$?
			log_build "$branch_name branch completed with status $branch_status"
			printf "%s\n" "$branch_status" > "$status_file"
			exit "$branch_status"
		} > "$log_file" 2>&1
	) &

	local branch_pid=$!
	log_build "Started $branch_name branch as pid $branch_pid (log: $log_file)"

	PARALLEL_BRANCH_NAMES+=("$branch_name")
	PARALLEL_BRANCH_PIDS+=("$branch_pid")
	PARALLEL_BRANCH_STATUS_FILES+=("$status_file")
	PARALLEL_BRANCH_STATUSES+=(-1)
	PARALLEL_BRANCH_LOG_FILES+=("$log_file")
	PARALLEL_BRANCH_RENDERED_LINES+=(0)
}

wait_for_parallel_branches() {
	local remaining=${#PARALLEL_BRANCH_PIDS[@]}
	local branch_index
	local use_parallel_renderer=false
	if parallel_renderer_enabled; then
		use_parallel_renderer=true
		print_parallel_render_header
	fi

	while (( remaining > 0 )); do
		for (( branch_index = 0; branch_index < ${#PARALLEL_BRANCH_PIDS[@]}; branch_index++ )); do
			if [[ ${PARALLEL_BRANCH_STATUSES[$branch_index]} -ne -1 ]]; then
				continue
			fi

			local status_file="${PARALLEL_BRANCH_STATUS_FILES[$branch_index]}"
			if [[ ! -f "$status_file" ]]; then
				continue
			fi

			local branch_status
			branch_status=$(cat "$status_file")
			wait "${PARALLEL_BRANCH_PIDS[$branch_index]}" 2> /dev/null || true
			PARALLEL_BRANCH_STATUSES[$branch_index]=$branch_status
			remaining=$((remaining - 1))

			if [[ $branch_status -eq 0 ]]; then
				if [[ $use_parallel_renderer != true ]]; then
					echo "${PARALLEL_BRANCH_NAMES[$branch_index]} branch finished successfully"
				fi
			else
				terminate_active_parallel_branches "$branch_index"
				if [[ $use_parallel_renderer = true ]]; then
					render_parallel_logs
				fi
				echo "Error: ${PARALLEL_BRANCH_NAMES[$branch_index]} branch failed with status $branch_status"
				print_parallel_log_tails
				cleanup_parallel_status_dir
				return "$branch_status"
			fi
		done

		if [[ $use_parallel_renderer = true ]]; then
			render_parallel_logs
		fi

		if (( remaining > 0 )); then
			sleep 1
		fi
	done

	if [[ $use_parallel_renderer = true ]]; then
		render_parallel_logs
		print_parallel_status_summary
	fi
	print_parallel_log_locations
	cleanup_parallel_status_dir
}

POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
  case $1 in
    -g|--game)
    	BUILD_ONLY_GAME=true
    	shift # past argument
    	;;
    --package-only)
		PACKAGE_ONLY=true
		shift # past argument
		;;
    -game_old|--game-old|--game_old)
		GAME_DIR=game_old
		GAME_BRANCH_LABEL="Game Old (Sokol)"
    	shift # past argument
    	;;
    -python|--python)
		set_blender_build_mode python
    	shift # past argument
    	;;
    -native|--native)
		set_blender_build_mode native
    	shift # past argument
    	;;
    -f|--file)
		run_args="$(resolve_blend_file_arg "$2")"
    	shift # past argument
    	shift # past argument
    	;;
    -*|--*)
    	echo "Unknown option $1"
    	exit 1
    	;;
    *)
    	POSITIONAL_ARGS+=("$1") # save positional arg
    	shift # past argument
    	;;
  esac
done

if [[ "$PACKAGE_ONLY" = "true" && "$BUILD_ONLY_GAME" = "true" ]]; then
	echo "Error: --package-only and --game are mutually exclusive"
	exit 1
fi

if [[ "$PACKAGE_ONLY" = "true" ]]; then
	echo "PACKAGE ONLY"
	prepare_flatbuffers_and_schemas || exit
	package_extension || exit
	exit 0
fi

if [[ "$BUILD_ONLY_GAME" != "true" && "$BLENDER_BUILD_MODE" = native && $OS != Mac && $OS != Linux ]]; then
	echo "Error: native Blender compilation is currently implemented only for macOS and Linux."
	echo "Use ./build.sh -python on $OS, or ./build.sh -g to build only the game."
	exit 1
fi

if [[ "$BUILD_ONLY_GAME" != "true" && "$BLENDER_BUILD_MODE" = native && $OS = Mac && $(uname -m) != arm64 ]]; then
	echo "Error: native Blender builds on macOS require Apple Silicon (arm64)."
	echo "Blender 5.1 does not provide precompiled macOS x64 libraries; use ./build.sh -python on Intel Macs."
	exit 1
fi

if [[ "$BUILD_ONLY_GAME" = "true" ]]; then
	if [[ "${BLENDER_LIVE_LINK_SKIP_GAME:-}" == "1" ]]; then
		echo "Skipping game build/run because BLENDER_LIVE_LINK_SKIP_GAME=1"
		exit 0
	fi

	run_game_build_and_launch
	exit $?
fi

echo "FULL BUILD ($BLENDER_BUILD_MODE)"
prepare_flatbuffers_and_schemas || exit

if [[ "${BLENDER_LIVE_LINK_SKIP_GAME:-}" == "1" ]]; then
	run_blender_side_build_and_launch || exit
	echo "Skipping game build/run because BLENDER_LIVE_LINK_SKIP_GAME=1"
	exit 0
fi

start_parallel_branch "$GAME_BRANCH_LABEL" run_game_build_and_launch || exit
start_parallel_branch "Blender" run_blender_side_build_and_launch || exit
wait_for_parallel_branches
