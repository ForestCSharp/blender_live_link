#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

BASE_DIR="${SCRIPT_DIR##*/}"
echo $BASE_DIR

# build flattbuffers 
cd $SCRIPT_DIR
rm -rf compiled_schemas
mkdir -p compiled_schemas/python
./flatbuffers/build/build.sh

# compile schema for python and cpp
./flatbuffers/build/flatc -o compiled_schemas/python --python blender_live_link.fbs 
./flatbuffers/build/flatc -o compiled_schemas/cpp --cpp blender_live_link.fbs 

# copy flatbuffers python package 
cp -a flatbuffers/python/flatbuffers/. compiled_schemas/python/flatbuffers

# Package up addon
cd $SCRIPT_DIR/..
rm $SCRIPT_DIR.zip
zip -r $SCRIPT_DIR.zip $BASE_DIR -x "$BASE_DIR/flatbuffers/*"
