
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

cd $SCRIPT_DIR

if [[ $OS = Windows ]]; then
  cmake .. -G "Visual Studio 17 2022"
  cmake --build . --target flatc
else
  cmake .. -G "Unix Makefiles"
  make -j
fi

