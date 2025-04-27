SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BASE_DIR="${SCRIPT_DIR##*/}"

cd $SCRIPT_DIR

pushd $SCRIPT_DIR/data/shaders/

OS_ARG=$1
echo $OS_ARG

if [[ $OS_ARG = Windows ]]; then
	SOKOL_SHDC="$SCRIPT_DIR/tools/sokol-tools/bin/win32/sokol-shdc.exe"
	SOKOL_SLANG=hlsl5
elif [[ $OS_ARG = Mac ]]; then
	SOKOL_SHDC="$SCRIPT_DIR/tools/sokol-tools/bin/osx_arm64/sokol-shdc"
	SOKOL_SLANG=metal_macos
elif [[ $OS_ARG = Linux ]]; then
	echo "Compiling Shaders for Linux: [TODO]"
else
	echo "Invalid OS Passed to game/compile_shaders.sh"
fi

for file in *.glsl; do
	output="$SCRIPT_DIR/bin/shaders/${file%.glsl}.compiled.h" 
	echo "compiling $file to $output"
	$SOKOL_SHDC -i $file -o $output --slang $SOKOL_SLANG --module ${file%.glsl}
done

popd
