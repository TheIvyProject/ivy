// H9.8: local C helper for `import c`; C ABI, so `extern "C"` in the .ivy.
#include <stdint.h>

int32_t a8_mul(int32_t a, int32_t b) {
    return a * b;
}
