cmake_minimum_required(VERSION 3.20)
project(ivy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

# H9.1: the Ivy runtime is Ivy source now (std/rt.ivy, ported from
# lib/ivyrt.c).  The oracle compiles it into a single COFF object,
# `build/rt.obj`, which replaces the `libivyrt` static archive.  It is
# linked by default into every Ivy executable; the linker drops unused
# sections from the object.  The command is declared after the `ivy`
# target because it runs that compiler.
set(IVYRT_OBJ "${CMAKE_BINARY_DIR}/rt.obj")

add_executable(ivy
    src/app/main.cpp
    src/parsing/lexer.cpp
    src/parsing/preprocessor.cpp
    src/parsing/parser.cpp
    src/parsing/module_io.cpp
    src/hir/hir_builder.cpp
    src/mir/mir_builder.cpp
    src/codegen/codegen.cpp
    src/mir/interpreter.cpp
)

target_include_directories(ivy PRIVATE src ${LLVM_INCLUDE_DIRS})

add_custom_command(
    OUTPUT "${IVYRT_OBJ}"
    COMMAND "$<TARGET_FILE:ivy>" -c "${CMAKE_SOURCE_DIR}/std/rt.ivy"
            -I "${CMAKE_SOURCE_DIR}/std" -o "${IVYRT_OBJ}"
    DEPENDS ivy "${CMAKE_SOURCE_DIR}/std/rt.ivy"
    COMMENT "Compiling std/rt.ivy -> rt.obj"
    VERBATIM)
add_custom_target(ivyrt ALL DEPENDS "${IVYRT_OBJ}")

# H9.1: Bake the absolute path to the runtime object into ivy.
target_compile_definitions(ivy PRIVATE
    IVYRT_LIB_PATH="${IVYRT_OBJ}")

# Suppress LLVM's C-style warnings when compiling our code.
target_compile_definitions(ivy PRIVATE ${LLVM_DEFINITIONS})

if(MSVC)
    target_compile_options(ivy PRIVATE /W4 /permissive- /FS
        /wd4244 /wd4267 /wd4146 /wd4291 /wd4624 /wd4996)
else()
    target_compile_options(ivy PRIVATE -Wall -Wextra -Wpedantic)
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

target_link_libraries(ivy PRIVATE ${LLVM_LIBS})
