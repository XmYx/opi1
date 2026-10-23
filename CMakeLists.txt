# op1-og-native build (owner: t3). No track edits this file:
#   - every src/<module>/**/*.cpp is compiled into the object library op1_<module>
#   - src/<module>/module.cmake (optional, owned by that module's track) adds link libraries and includes
#   - every third_party/<lib>/CMakeLists.txt is added (each defines its own target)
#   - every tests/<dir>/*.cpp is one test executable, registered with ctest as <dir>/<name>
# Build with ./build.sh (sets up the local ALSA/SDL3 headers first).
cmake_minimum_required(VERSION 3.20)
project(op1_og_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")  # symbols kept for profiling and crash reports
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

option(OP1_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(OP1_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(Threads REQUIRED)

file(GLOB OP1_THIRD_PARTY CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/*/CMakeLists.txt)
foreach(f ${OP1_THIRD_PARTY})
  get_filename_component(d ${f} DIRECTORY)
  add_subdirectory(${d})
endforeach()

set(OP1_MODULES core platform dsp tape seq midi state gui assets)
set(OP1_MODULE_TARGETS "")
foreach(m ${OP1_MODULES})
  file(GLOB_RECURSE src CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/${m}/*.cpp ${CMAKE_SOURCE_DIR}/src/${m}/*.c)
  add_library(op1_${m} OBJECT ${src})
  target_include_directories(op1_${m} PUBLIC ${CMAKE_SOURCE_DIR}/include PRIVATE ${CMAKE_SOURCE_DIR}/src/${m})
  target_link_libraries(op1_${m} PUBLIC Threads::Threads)
  if(EXISTS ${CMAKE_SOURCE_DIR}/src/${m}/module.cmake)
    include(${CMAKE_SOURCE_DIR}/src/${m}/module.cmake)
  endif()
  list(APPEND OP1_MODULE_TARGETS op1_${m})
endforeach()

add_executable(op1-og ${CMAKE_SOURCE_DIR}/src/app/main.cpp)
target_link_libraries(op1-og PRIVATE ${OP1_MODULE_TARGETS})

enable_testing()
file(GLOB OP1_TEST_DIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*)
foreach(d ${OP1_TEST_DIRS})
  if(IS_DIRECTORY ${d})
    get_filename_component(dir ${d} NAME)
    file(GLOB tests CONFIGURE_DEPENDS ${d}/*.cpp)
    foreach(t ${tests})
      get_filename_component(name ${t} NAME_WE)
      add_executable(test_${dir}_${name} ${t})
      target_include_directories(test_${dir}_${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
      target_link_libraries(test_${dir}_${name} PRIVATE ${OP1_MODULE_TARGETS})
      add_test(NAME ${dir}/${name} COMMAND test_${dir}_${name})
    endforeach()
  endif()
endforeach()
