#!/usr/bin/env bash

# Running ./build.sh builds native blender integration by default, then builds and runs game
# Running ./build.sh -python builds blender add-on, installs it to blender, and launches blender, then builds and runs game
# Running ./build.sh game only rebuilds the game and runs it

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
BLENDER_BUILD_MODE=native
BLENDER_BUILD_MODE_WAS_SET=false
NATIVE_BLENDER_BINARY="$SCRIPT_DIR/blend_src/build_macos_lite/bin/Blender.app/Contents/MacOS/Blender"
NATIVE_BLENDER_USER_DIR="$SCRIPT_DIR/blend_src/blender_user"
EXTENSION_ZIP_PATH="$SCRIPT_DIR.zip"

run_args="$SCRIPT_DIR/blend_files/test_file.blend"

set_blender_build_mode() {
	local requested_mode=$1
	if [[ $BLENDER_BUILD_MODE_WAS_SET = true && $BLENDER_BUILD_MODE != "$requested_mode" ]]; then
		echo "Error: -python and -native are mutually exclusive"
		exit 1
	fi

	BLENDER_BUILD_MODE=$requested_mode
	BLENDER_BUILD_MODE_WAS_SET=true
}

package_extension() {
	cd $SCRIPT_DIR/..
	rm -f "$EXTENSION_ZIP_PATH"

	if [[ $OS = Windows ]]; then
		7z a -tzip "$EXTENSION_ZIP_PATH" $BASE_DIR -w $BASE_DIR/ -r \
			-x!"$BASE_DIR/flatbuffers/*" \
			-x!"$BASE_DIR/.git/*" \
			-x!"$BASE_DIR/game/*" \
			-x!"$BASE_DIR/blend_files/*" \
			-x!"$BASE_DIR/blend_src/*" \
			-x!"$BASE_DIR/compiled_schemas/cpp/*" \
			-x!"$BASE_DIR/docs/*" \
			-x!"$BASE_DIR/build.sh" \
			-x!"$BASE_DIR/build_blend_src.sh" \
			-x!"$BASE_DIR/clean_blend_src.sh" \
			-x!"$BASE_DIR/README.md" \
			-x!"$BASE_DIR/TODO.txt" \
			-x!"$BASE_DIR/.gitignore" \
			-x!"$BASE_DIR/blender_live_link.fbs" \
			-x!"$BASE_DIR/.DS_Store" \
			-x!"*/.DS_Store"
	else
		zip -r "$EXTENSION_ZIP_PATH" $BASE_DIR \
			-x "$BASE_DIR/flatbuffers/*" \
			-x "$BASE_DIR/.git/*" \
			-x "$BASE_DIR/game/*"\
			-x "$BASE_DIR/blend_files/*" \
			-x "$BASE_DIR/blend_src/*" \
			-x "$BASE_DIR/compiled_schemas/cpp/*" \
			-x "$BASE_DIR/docs/*" \
			-x "$BASE_DIR/build.sh" \
			-x "$BASE_DIR/build_blend_src.sh" \
			-x "$BASE_DIR/clean_blend_src.sh" \
			-x "$BASE_DIR/README.md" \
			-x "$BASE_DIR/TODO.txt" \
			-x "$BASE_DIR/.gitignore" \
			-x "$BASE_DIR/blender_live_link.fbs" \
			-x "$BASE_DIR/.DS_Store" \
			-x "*/.DS_Store"
	fi

	cd $SCRIPT_DIR
}

install_and_launch_installed_blender() {
	local install_args=(--command extension install-file "$EXTENSION_ZIP_PATH" --repo user_default --enable)

	if [[ $OS = Windows ]]; then
		# Note: blender.exe should be on system path on windows
		# kill previous blender instances
		taskkill.exe //F //IM blender.exe
		sleep 0.5
		# install add-on and wait for completion
		blender.exe "${install_args[@]}"
		sleep 0.5
		# open blender to specified map file
		start "" blender.exe $run_args
	elif [[ $OS = Mac ]]; then
		# kill previous blender instances
		killall Blender
		# install add-on and wait for completion
		/Applications/Blender.app/Contents/MacOS/Blender "${install_args[@]}"
		sleep 0.5
		# open blender without waiting for completion
		open  /Applications/Blender.app --args $run_args
	fi
}

with_native_blender_profile() {
	mkdir -p "$NATIVE_BLENDER_USER_DIR/config" "$NATIVE_BLENDER_USER_DIR/scripts" "$NATIVE_BLENDER_USER_DIR/datafiles"
	BLENDER_USER_CONFIG="$NATIVE_BLENDER_USER_DIR/config" \
	BLENDER_USER_SCRIPTS="$NATIVE_BLENDER_USER_DIR/scripts" \
	BLENDER_USER_DATAFILES="$NATIVE_BLENDER_USER_DIR/datafiles" \
	"$@"
}

install_and_launch_native_blender() {
	if [[ $OS != Mac ]]; then
		echo "Warning: native Blender launch is currently implemented only for Mac."
		echo "Warning: skipping local Blender extension install/launch on $OS."
		return
	fi

	if [[ ! -x "$NATIVE_BLENDER_BINARY" ]]; then
		echo "Error: native Blender binary was not found at $NATIVE_BLENDER_BINARY"
		echo "Expected build_blend_src.sh to build it before extension install/launch."
		exit 1
	fi

	local install_args=(--command extension install-file "$EXTENSION_ZIP_PATH" --repo user_default --enable)

	echo "Installing extension into local Blender profile at $NATIVE_BLENDER_USER_DIR"
	with_native_blender_profile "$NATIVE_BLENDER_BINARY" "${install_args[@]}"
	sleep 0.5

	echo "Launching local Blender at $NATIVE_BLENDER_BINARY"
	with_native_blender_profile "$NATIVE_BLENDER_BINARY" "$run_args" &
}

POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
  case $1 in
    -g|--game)
    	BUILD_ONLY_GAME=true
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
		run_args="$SCRIPT_DIR/blend_files/$2"
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

if ! [ "$BUILD_ONLY_GAME" = "true" ]; then

	echo "FULL BUILD ($BLENDER_BUILD_MODE)"

	if [[ $BLENDER_BUILD_MODE = native ]]; then
		"$SCRIPT_DIR/build_blend_src.sh"
	fi

	# build flattbuffers, passing in OS as first arg
	cd $SCRIPT_DIR
	rm -rf compiled_schemas
	mkdir -p compiled_schemas/cpp
	./flatbuffers/build.sh $OS


	if [[ $OS = Windows ]]; then
	  FLATC_BINARY=flatbuffers/build/Debug/flatc.exe
	else
	  FLATC_BINARY=flatbuffers/build/flatc
	fi

	# compile schema for cpp
	touch compiled_schemas/__init__.py
	echo FLATC IS $FLATC_BINARY
	$FLATC_BINARY -o compiled_schemas/cpp --cpp blender_live_link.fbs 

	mkdir -p compiled_schemas/python
	touch compiled_schemas/python/__init__.py
	$FLATC_BINARY -o compiled_schemas/python --python blender_live_link.fbs

	# copy flatbuffers python package
	cp -a flatbuffers/python/flatbuffers/. compiled_schemas/python/flatbuffers

	package_extension

	if [[ $BLENDER_BUILD_MODE = python ]]; then
		install_and_launch_installed_blender
	else
		install_and_launch_native_blender
	fi
fi

# Compile game, passing in OS as first arg
cd $SCRIPT_DIR/game
./build.sh $OS
