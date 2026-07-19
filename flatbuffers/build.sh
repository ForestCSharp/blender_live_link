
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build/" || exit 1

OS_ARG=$1

if [[ $OS_ARG = Windows ]]; then
  cmake "$SCRIPT_DIR"
  cmake --build . --config Release --target flatc
elif [[ $OS_ARG = Mac ]]; then
  cmake "$SCRIPT_DIR" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
  cmake --build . --target flatc
elif [[ $OS_ARG = Linux ]]; then
  cmake "$SCRIPT_DIR" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
  cmake --build . --target flatc
else
	echo "Invalid OS Passed to flatbuffers/build.sh"
	exit 1
fi
