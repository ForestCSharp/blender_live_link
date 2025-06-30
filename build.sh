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
		-x!"$BASE_DIR/game/*"
else
	zip -r $SCRIPT_DIR.zip $BASE_DIR \
		-x "$BASE_DIR/flatbuffers/*" \
		-x "$BASE_DIR/.git/*" \
		-x "$BASE_DIR/game/*"
fi

install_args="--command extension install-file blender_live_link.zip --repo user_default --enable"
run_args="$SCRIPT_DIR/blend_files/test_file.blend"

if [[ $OS = Windows ]]; then
	# Note: blender.exe should be on system path
	blender.exe $install_args
	sleep 0.5
	start "" blender.exe $run_args
elif [[ $OS = Mac ]]; then
	# install add-on and wait for completion
	open -W -n /Applications/Blender.app --args $install_args 
	sleep 0.5

	# open blender without waiting for completion
	open  /Applications/Blender.app --args $run_args
	#open /Applications/Blender.app 
fi

# Compile game, passing in OS as first arg
cd $SCRIPT_DIR/game
./build.sh $OS
