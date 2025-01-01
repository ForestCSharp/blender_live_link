
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

mkdir -p $SCRIPT_DIR/build
cd $SCRIPT_DIR/build/

OS_ARG=$1

if [[ $OS_ARG = Windows ]]; then
  cmake .. -G "Visual Studio 17 2022"
  cmake --build . --target flatc
elif [[ $OS_ARG = Mac ]]; then
  cmake .. -G "Unix Makefiles"
  cmake --build . --target flatc
elif [[ $OS_ARG = Linux ]]; then
  cmake .. -G "Unix Makefiles"
  cmake --build . --target flatc
else
	echo "Invalid OS Passed to flatbuffers/build.sh"
fi

