#!/usr/bin/env bash

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

# build flattbuffers, passing in OS as first arg
cd $SCRIPT_DIR
rm -rf compiled_schemas
mkdir -p compiled_schemas/python
./flatbuffers/build.sh $OS


if [[ $OS = Windows ]]; then
  FLATC_BINARY=flatbuffers/build/Debug/flatc.exe
else
  FLATC_BINARY=flatbuffers/build/flatc
fi

# compile schema for python and cpp
touch compiled_schemas/__init__.py
touch compiled_schemas/python/__init__.py
echo FLATC IS $FLATC_BINARY
$FLATC_BINARY -o compiled_schemas/python --python blender_live_link.fbs 
$FLATC_BINARY -o compiled_schemas/cpp --cpp blender_live_link.fbs 

# copy flatbuffers python package 
cp -a flatbuffers/python/flatbuffers/. compiled_schemas/python/flatbuffers

# Package up addon
cd $SCRIPT_DIR/..
rm $SCRIPT_DIR.zip

if [[ $OS = Windows ]]; then
	7z a -tzip $SCRIPT_DIR.zip $BASE_DIR -w $BASE_DIR/ -r \
		-x!"$BASE_DIR/flatbuffers/*" \
		-x!"$BASE_DIR/.git/*" \
		-x!"$BASE_DIR/game/*" \
		-x!"$BASE_DIR/blend_files/*"
else
	zip -r $SCRIPT_DIR.zip $BASE_DIR \
		-x "$BASE_DIR/flatbuffers/*" \
		-x "$BASE_DIR/.git/*" \
		-x "$BASE_DIR/game/*"\
		-x "$BASE_DIR/blend_files/*"
fi

install_args="--command extension install-file blender_live_link.zip --repo user_default --enable"

# Default to test_file but check first arg for a specific blend file
run_args="$SCRIPT_DIR/blend_files/test_file.blend"
if [ -n "$1" ]; then
	run_args="$SCRIPT_DIR/blend_files/$1"
fi

if [[ $OS = Windows ]]; then
	# Note: blender.exe should be on system path on windows
	# kill previous blender instances
	taskkill.exe //F //IM blender.exe
	sleep 0.5
	# install add-on and wait for completion
	blender.exe $install_args
	sleep 0.5
	# open blender to specified map file
	start "" blender.exe $run_args
elif [[ $OS = Mac ]]; then
	# kill previous blender instances
	killall Blender	
	# install add-on and wait for completion
	/Applications/Blender.app/Contents/MacOS/Blender $install_args 
	sleep 0.5
	# open blender without waiting for completion
	open  /Applications/Blender.app --args $run_args
fi

# Compile game, passing in OS as first arg
cd $SCRIPT_DIR/game
./build.sh $OS
