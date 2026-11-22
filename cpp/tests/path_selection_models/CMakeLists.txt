cmake_minimum_required(VERSION 3.20)
project(MineTeleopPathSelectionModels LANGUAGES CXX)

# Standalone, no FetchContent, runtime library, OpenSSL, sockets or hardware.
# The main project's CMake remains untouched during concurrent PR24 integration.
enable_testing()
foreach(layer IN ITEMS stop feedback switch)
  add_executable(path-${layer}-model-tests ${layer}_model_tests.cpp)
  target_compile_features(path-${layer}-model-tests PRIVATE cxx_std_20)
  target_include_directories(path-${layer}-model-tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
  if(MSVC)
    target_compile_options(path-${layer}-model-tests PRIVATE /W4 /WX)
  else()
    target_compile_options(path-${layer}-model-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME path-${layer}-model-tests COMMAND path-${layer}-model-tests)
  set_tests_properties(path-${layer}-model-tests PROPERTIES TIMEOUT 10)
endforeach()
