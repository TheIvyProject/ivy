cmake_minimum_required(VERSION 3.20)
project(ivyc LANGUAGES C CXX)

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

# C5: Ivy runtime static library (libivyrt).
# Written in C (bootstrap).  Contains the __ivy_* runtime symbols
# (allocator, I/O, panic).  Linked by default into every Ivy
# executable; the linker drops unused symbols from the archive.
add_library(ivyrt STATIC lib/ivyrt.c)
target_include_directories(ivyrt PUBLIC lib)
if(MSVC)
    target_compile_options(ivyrt PRIVATE /W4)
else()
    target_compile_options(ivyrt PRIVATE -Wall -Wextra -Wpedantic)
endif()
# Expose the built archive path to ivyc via a compile definition so
# linkExecutable() can find libivyrt.lib / libivyrt.a at runtime.
# The generator expression $<TARGET_FILE:ivyrt> is evaluated by the
# build system at compile time of ivyc, baking the absolute path.
# (Applied after add_executable below — target must exist first.)

add_executable(ivyc
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

target_include_directories(ivyc PRIVATE src ${LLVM_INCLUDE_DIRS})

# C5: Bake the absolute path to the built libivyrt archive into ivyc.
target_compile_definitions(ivyc PRIVATE
    IVYRT_LIB_PATH="$<TARGET_FILE:ivyrt>")

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
