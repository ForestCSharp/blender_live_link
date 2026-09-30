#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "$(uname -s)" in
  Darwin) platform=Mac; system_libs=(-framework Cocoa -framework IOKit -framework QuartzCore) ;;
  Linux) platform=Linux; system_libs=(-ldl -pthread) ;;
  *) echo 'This test runner supports macOS and Linux.' >&2; exit 1 ;;
esac
[[ -f /tmp/weapon_fixture.bin ]] || { echo 'Run weapon_export_blender.py first.' >&2; exit 1; }
GAME_BUILD_CONFIG=Debug ./build.sh "$platform" -norun
includes=(-I src -I extern -I extern/glfw/include -I extern/imgui -I data
          -I data/shaders -I bin/shaders -I ../flatbuffers/include -I ../compiled_schemas/cpp)
if [[ -n "${VULKAN_SDK:-}" ]]; then includes+=(-I "$VULKAN_SDK/include"); fi
cache="bin/build/$platform/Debug"
clang++ -std=c++20 -O0 -g -Wno-c99-designator -DWITH_DEBUG_UI=0 -DGAME_ENABLE_VALIDATION=1 \
  tests/weapon_runtime_tests.cpp "${includes[@]}" \
  "$cache/libglfw.a" "$cache/libvma.a" "$cache/libjolt.a" "${system_libs[@]}" \
  -o /tmp/weapon_runtime_tests
/tmp/weapon_runtime_tests
