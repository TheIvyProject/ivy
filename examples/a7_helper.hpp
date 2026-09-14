// A7: C++ helper header for import cpp test
// Defines a simple extern "C" function that Ivy can call directly.
#pragma once
#include <cstdint>

extern "C" int32_t a7_add(int32_t a, int32_t b) {
    return a + b;
}
