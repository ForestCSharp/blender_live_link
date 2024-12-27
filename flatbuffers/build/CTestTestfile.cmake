# CMake generated Testfile for 
# Source directory: C:/Users/fores/Desktop/blender_live_link/flatbuffers
# Build directory: C:/Users/fores/Desktop/blender_live_link/flatbuffers/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[flattests]=] "C:/Users/fores/Desktop/blender_live_link/flatbuffers/build/Debug/flattests.exe")
  set_tests_properties([=[flattests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;688;add_test;C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[flattests]=] "C:/Users/fores/Desktop/blender_live_link/flatbuffers/build/Release/flattests.exe")
  set_tests_properties([=[flattests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;688;add_test;C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[flattests]=] "C:/Users/fores/Desktop/blender_live_link/flatbuffers/build/MinSizeRel/flattests.exe")
  set_tests_properties([=[flattests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;688;add_test;C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[flattests]=] "C:/Users/fores/Desktop/blender_live_link/flatbuffers/build/RelWithDebInfo/flattests.exe")
  set_tests_properties([=[flattests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;688;add_test;C:/Users/fores/Desktop/blender_live_link/flatbuffers/CMakeLists.txt;0;")
else()
  add_test([=[flattests]=] NOT_AVAILABLE)
endif()
