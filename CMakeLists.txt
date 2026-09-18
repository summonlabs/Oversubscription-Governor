cmake_minimum_required(VERSION 3.20)
project(OversubscriptionGovernor VERSION 1.0.0 LANGUAGES CXX)

option(OVERSUB_BUILD_TESTS "Build the full test suite" ON)
option(OVERSUB_BUILD_DIST "Build the multiprocess coordinator/worker tool" ON)
option(OVERSUB_BUILD_EXAMPLES "Build example programs" ON)
option(OVERSUB_BUILD_CLI "Build the command-line tool" ON)
option(OVERSUB_BUILD_BENCHMARKS "Build benchmarks" ON)
option(OVERSUB_STRICT_WARNINGS "Treat warnings as errors with strict flags" ON)
option(OVERSUB_ENABLE_ASAN "Build with AddressSanitizer" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Platform compiler flags. Windows-first MSVC; portable GCC/Clang.
if(MSVC)
  add_compile_options(/W4)
  if(OVERSUB_STRICT_WARNINGS)
    add_compile_options(/WX /permissive-)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/EHsc)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(OVERSUB_STRICT_WARNINGS)
    add_compile_options(-Werror)
  endif()
endif()

if(OVERSUB_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address /INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

function(oversub_target tgt)
  target_include_directories(${tgt} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(${tgt} PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4)
    if(OVERSUB_STRICT_WARNINGS)
      target_compile_options(${tgt} PRIVATE /WX /permissive-)
    endif()
  endif()
endfunction()

add_library(OversubscriptionGovernor
  src/checked.cpp
  src/clock.cpp
  src/digest.cpp
  src/status.cpp
  src/evidence.cpp
  src/policy.cpp
  src/engine.cpp
  src/state.cpp
  src/persistence.cpp
  src/governor.cpp
  src/net.cpp
  src/process.cpp
  src/synth.cpp
)
add_library(OversubscriptionGovernor::OversubscriptionGovernor ALIAS OversubscriptionGovernor)
oversub_target(OversubscriptionGovernor)
set_target_properties(OversubscriptionGovernor PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

find_package(Threads REQUIRED)
target_link_libraries(OversubscriptionGovernor PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(OversubscriptionGovernor PUBLIC ws2_32)
endif()
target_compile_definitions(OversubscriptionGovernor PUBLIC
  OVERSUB_VERSION_STRING="${PROJECT_VERSION}")

# Place all executables in a single binary directory so the multiprocess proof
# can locate its helper processes next to the test executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(CONFIG Debug Release RelWithDebInfo MinSizeRel)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# ---- Installation / package export ----
install(TARGETS OversubscriptionGovernor
  EXPORT OversubscriptionGovernorTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/oversub DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT OversubscriptionGovernorTargets
  FILE OversubscriptionGovernorTargets.cmake
  NAMESPACE OversubscriptionGovernor::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OversubscriptionGovernor)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/OversubscriptionGovernorConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/OversubscriptionGovernorConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OversubscriptionGovernor)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/OversubscriptionGovernorConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/OversubscriptionGovernorConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/OversubscriptionGovernorConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OversubscriptionGovernor)

if(OVERSUB_BUILD_DIST)
  add_subdirectory(dist)
endif()
if(OVERSUB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(OVERSUB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(OVERSUB_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(OVERSUB_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
