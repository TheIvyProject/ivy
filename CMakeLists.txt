cmake_minimum_required(VERSION 3.20)
project(ivyc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ivyc
    src/main.cpp
    src/lexer.cpp
    src/preprocessor.cpp
    src/parser.cpp
    src/hir_builder.cpp
    src/mir_builder.cpp
    src/codegen.cpp
)

target_include_directories(ivyc PRIVATE src)

if(MSVC)
    target_compile_options(ivyc PRIVATE /W4 /permissive-)
else()
    target_compile_options(ivyc PRIVATE -Wall -Wextra -Wpedantic)
endif()