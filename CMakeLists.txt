cmake_minimum_required(VERSION 3.20)
project(ivyc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 8.1: Find LLVM for native object file emission.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# NOTE: LLVM's CMake exports (LLVMExports.cmake) bake a path to
# diaguids.lib into the INTERFACE_LINK_LIBRARIES of the
# LLVMDebugInfoPDB target. On this machine that resolved to the
# VS2019 Professional copy, which is a corrupt 140-byte placeholder.
# We patched LLVMExports.cmake directly to point at the VS18 copy
# (2.3 MB, valid). See git history / session notes for details.

add_executable(ivyc
    src/app/main.cpp
    src/parsing/lexer.cpp
    src/parsing/preprocessor.cpp
    src/parsing/parser.cpp
    src/hir/hir_builder.cpp
    src/mir/mir_builder.cpp
    src/codegen/codegen.cpp
    src/mir/interpreter.cpp
)

target_include_directories(ivyc PRIVATE src ${LLVM_INCLUDE_DIRS})

# Suppress LLVM's C-style warnings when compiling our code.
target_compile_definitions(ivyc PRIVATE ${LLVM_DEFINITIONS})

if(MSVC)
    target_compile_options(ivyc PRIVATE /W4 /permissive- /FS
        /wd4244 /wd4267 /wd4146 /wd4291 /wd4624 /wd4996)
else()
    target_compile_options(ivyc PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 8.1: Link LLVM libraries needed for object file emission.
# llvm_map_components_to_libnames resolves the full transitive
# dependency set (LLVM static libs are not self-contained).
llvm_map_components_to_libnames(LLVM_LIBS
    Core
    IRReader
    Support
    Target
    TargetParser
    native
    AsmPrinter
    MC
    MCParser
    CodeGen
    Analysis
)

target_link_libraries(ivyc PRIVATE ${LLVM_LIBS})
