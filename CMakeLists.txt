#[[
   Copyright 2020-2021 The Silkworm Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
]]

cmake_minimum_required(VERSION 3.13)

find_package(Git)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules")
  # Update submodules
  option(GIT_SUBMODULE "Check submodules during build" ON)
  if(GIT_SUBMODULE)
    message(STATUS "Submodule update")
    execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    RESULT_VARIABLE GIT_SUBMOD_RESULT)
    if(NOT GIT_SUBMOD_RESULT EQUAL "0")
       message(FATAL_ERROR "git submodule update --init failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
    endif()
  endif()
else()
  message(FATAL_ERROR "Git tool is required.")
endif()

get_directory_property(SILKWORM_HAS_PARENT PARENT_DIRECTORY)

if(NOT SILKWORM_HAS_PARENT)
  include(evmone/cmake/cable/bootstrap.cmake)
  include(CableBuildType)
  cable_set_build_type(DEFAULT Release CONFIGURATION_TYPES Release Debug)

  if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain.cmake CACHE FILEPATH "" FORCE)
  endif()

  include(evmone/cmake/cable/HunterGate.cmake)
  HunterGate(
    URL "https://github.com/cpp-pm/hunter/archive/v0.23.288.tar.gz"
    SHA1 "6c9b2bc606d86ae31f96a62fc68f0a593024815b"
    LOCAL
    )
endif()

#[[
If you want to include Silkworm into your CMake project,
add the following lines before the project command of your root CMakeLists.txt:

-----------------------------------------------------------------------------------------------------
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/silkworm/cmake/toolchain.cmake CACHE FILEPATH "" FORCE)

include(silkworm/evmone/cmake/cable/HunterGate.cmake)
HunterGate(
  URL "https://github.com/cpp-pm/hunter/archive/v0.23.288.tar.gz"
  SHA1 "6c9b2bc606d86ae31f96a62fc68f0a593024815b"
  FILEPATH "${CMAKE_SOURCE_DIR}/silkworm/cmake/Hunter/config.cmake"
)
-----------------------------------------------------------------------------------------------------

(You can update the Hunter version, but keep the FILEPATH option.)
]]

project(silkworm)

option(SILKWORM_CORE_ONLY "Only build Silkworm Core" OFF)

include(cmake/Hunter/core_packages.cmake)
if(NOT SILKWORM_CORE_ONLY)
  include(cmake/Hunter/extra_packages.cmake)
endif()

option(SILKWORM_CLANG_COVERAGE "Clang instrumentation for code coverage reports" OFF)

if(SILKWORM_CLANG_COVERAGE)
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang$" AND CMAKE_BUILD_TYPE STREQUAL "Release")
  # useful for performance profiling
  add_compile_options(-gline-tables-only)
endif()

# GMP
find_path(GMP_INCLUDE_DIR NAMES gmp.h)
if(MSVC)
  find_library(GMP_LIBRARY mpir)
else()
  find_library(GMP_LIBRARY gmp)
endif()
if(GMP_LIBRARY MATCHES ${CMAKE_SHARED_LIBRARY_SUFFIX})
  set(gmp_library_type SHARED)
else()
  set(gmp_library_type STATIC)
endif()
message(STATUS "GMP: ${GMP_LIBRARY}, ${GMP_INCLUDE_DIR}")
add_library(gmp ${gmp_library_type} IMPORTED)
set_target_properties(
  gmp PROPERTIES
  IMPORTED_LOCATION ${GMP_LIBRARY}
  INTERFACE_INCLUDE_DIRECTORIES ${GMP_INCLUDE_DIR}
)

# secp256k1
add_library(secp256k1 secp256k1/src/secp256k1.c)

if(MSVC)
  target_link_libraries(secp256k1 PRIVATE gmp)
  target_compile_definitions(secp256k1 PUBLIC USE_NUM_GMP USE_FIELD_INV_NUM USE_SCALAR_INV_NUM)
  target_compile_definitions(secp256k1 PUBLIC USE_FIELD_10X26 USE_SCALAR_8X32)
else()
  target_compile_definitions(secp256k1 PUBLIC USE_NUM_NONE USE_FIELD_INV_BUILTIN USE_SCALAR_INV_BUILTIN)
  target_compile_definitions(secp256k1 PUBLIC USE_FIELD_5X52 USE_SCALAR_4X64 HAVE___INT128)
endif()
target_compile_definitions(secp256k1 PUBLIC ECMULT_WINDOW_SIZE=15 ECMULT_GEN_PREC_BITS=4 USE_ENDOMORPHISM)
target_compile_definitions(secp256k1 PUBLIC ENABLE_MODULE_RECOVERY)

target_include_directories(secp256k1 PRIVATE secp256k1 INTERFACE secp256k1/include)

# libff
set(CURVE "ALT_BN128" CACHE STRING "" FORCE)
option(WITH_PROCPS "" OFF)
option(IS_LIBFF_PARENT "" OFF)
if(MSVC)
  option(MPIR_INSTEAD_OF_GMP "" ON)
endif()
add_subdirectory(libff)

if(NOT SILKWORM_CORE_ONLY)
  # CBOR
  add_subdirectory(cbor-cpp)

  # LMDB
  add_library(lmdb lmdb-go/lmdb/lmdb.h
                  lmdb-go/lmdb/mdb.c
                  lmdb-go/lmdb/midl.c
                  lmdb-go/lmdb/midl.h)
  target_include_directories(lmdb PUBLIC lmdb-go)
endif()

# Warnings options
if(MSVC)

  add_definitions(-D_WIN32_WINNT=0x0602) # Min Windows 8
  add_definitions(-DVC_EXTRALEAN)        # Process windows headers faster ...
  add_definitions(-DWIN32_LEAN_AND_MEAN) # ... and prevent winsock mismatch with Boost's
  add_definitions(-DNOMINMAX)            # Prevent MSVC to tamper with std::min/std::max

  # Abseil triggeres some deprecation warnings
  add_compile_definitions(_SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING)
  add_compile_definitions(_SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING)

  add_compile_options(/wd5030) # Silence warnings about GNU attributes
  add_compile_options(/wd4324) # Silence warning C4324: 'xxx': structure was padded due to alignment specifier
  add_compile_options(/wd4068) # Silence warning C4068: unknown pragma
  add_compile_options(/W4)

  add_link_options(/ignore:4099)
else()
  add_compile_options(-Wall -Wextra -Werror -Wno-missing-field-initializers -Wimplicit-fallthrough)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wno-attributes)
endif()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  add_compile_definitions(_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS)
  add_compile_options(-Wthread-safety)
endif()

# evmone with dependencies
add_compile_definitions(EVMC_LOADER_MOCK)
add_subdirectory(evmone/evmc)
add_subdirectory(keccak)
find_package(intx CONFIG REQUIRED)

add_library(evmone evmone/lib/evmone/analysis.cpp
                   evmone/lib/evmone/analysis.hpp
                   evmone/lib/evmone/execution_state.hpp
                   evmone/lib/evmone/instructions_calls.cpp
                   evmone/lib/evmone/instructions.cpp
                   evmone/lib/evmone/instructions.hpp
                   evmone/lib/evmone/limits.hpp
                   evmone/lib/evmone/opcodes_helpers.h)

target_include_directories(evmone PUBLIC evmone/lib)
target_link_libraries(evmone PUBLIC evmc intx::intx PRIVATE keccak)

if(MSVC)
  target_compile_options(evmone PRIVATE /EHsc)
else()
  target_compile_options(evmone PRIVATE -fno-exceptions)
endif()

# Silkworm itself
add_subdirectory(core)

if(NOT SILKWORM_CORE_ONLY)
  add_subdirectory(db)
  add_subdirectory(tg_api)
endif()

if(NOT SILKWORM_HAS_PARENT)
  add_subdirectory(cmd)
endif()

option(SILKWORM_WASM_API "Build WebAssembly API" OFF)
if(SILKWORM_WASM_API)
  add_subdirectory(wasm)
endif()
