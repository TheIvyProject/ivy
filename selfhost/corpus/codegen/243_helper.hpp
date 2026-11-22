// H9.8: local C++ helper for `import cpp`; gives the linker a real object.
#include <cstdint>

extern "C" int32_t a8_add(int32_t a, int32_t b) {
    return a + b;
}
