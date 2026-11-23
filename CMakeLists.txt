cmake_minimum_required(VERSION 3.29)

# c2y and the defer TS take clang 22, newer than the one Xcode ships.
if(APPLE AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  find_program(CMAKE_C_COMPILER clang
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
endif()

project(cilo VERSION 0.1.0 LANGUAGES C)

# cmake knows no standard past C23, so the flags are given by hand.
set(CILO_C2Y -std=c2y -fdefer-ts)
include(CheckCCompilerFlag)
check_c_compiler_flag(-fdefer-ts CILO_HAS_DEFER)
if(NOT CILO_HAS_DEFER)
  message(FATAL_ERROR
    "cilo is C2y with the defer TS, set CC to clang 22 or later")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})

option(CILO_BUILD_TESTS "build the cilo tests and benchmarks" ${PROJECT_IS_TOP_LEVEL})

add_library(cilo STATIC src/cilo.c src/heap.c src/map.c)
find_package(Threads REQUIRED)
target_link_libraries(cilo PUBLIC Threads::Threads)
add_library(cilo::cilo ALIAS cilo)
target_include_directories(cilo PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_options(cilo PRIVATE ${CILO_C2Y} -Wall -Wextra -Wpedantic)

# apple clang's machine outliner turns the inlined fast paths of cilo back into calls inside hot loops.
option(CILO_NO_OUTLINE "build cilo and its users with -mno-outline where supported" ON)
check_c_compiler_flag(-mno-outline CILO_HAS_NO_OUTLINE)
if(CILO_NO_OUTLINE AND CILO_HAS_NO_OUTLINE)
  target_compile_options(cilo PUBLIC -mno-outline)
endif()
set_target_properties(cilo PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CILO_BUILD_TESTS)
  enable_testing()
  foreach(name tests/cilo_test examples/tour)
    get_filename_component(target ${name} NAME)
    add_executable(${target} ${name}.c)
    target_link_libraries(${target} PRIVATE cilo)
    target_compile_options(${target} PRIVATE ${CILO_C2Y} -Wall -Wextra -Wpedantic)
    add_test(NAME ${target} COMMAND ${target})
  endforeach()
  add_executable(cilo_bench bench/bench.c)
  target_link_libraries(cilo_bench PRIVATE cilo)
  target_compile_options(cilo_bench PRIVATE ${CILO_C2Y} -O2 -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS cilo EXPORT cilo-targets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cilo-targets NAMESPACE cilo:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cilo)
configure_package_config_file(cmake/cilo-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cilo-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cilo)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cilo-config.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cilo)
