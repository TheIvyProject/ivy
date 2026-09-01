// ivyc/lib/ivy.h — standard header for the Ivy subset.
//
// This file provides declarations that Ivy programs can pull in via
// `#include <ivy.h>`. It is processed by the ivyc preprocessor and
// compiled as Ivy source code.
//
// 8.5: ivy::print / ivy::println — type-aware printing (no printf needed).
//       These are resolved as builtins by both the interpreter and codegen.
//
// Note: ivy::string, ivy::vector<T>, and ivy::result<T,E> are planned
// but require additional language features (templates in headers, const
// methods, inline functions) that are not yet fully supported.

#ifndef IVY_LIB_IVY_H
#define IVY_LIB_IVY_H

// --- C runtime linkage (for codegen mode) ---
extern "C" {
    void* malloc(unsigned long n);
    void free(void* p);
    void ivy_print_int(int v);
    void ivy_print_float(double v);
    void ivy_print_str(const char* s);
    void ivy_print_char(char c);
    void ivy_println_int(int v);
    void ivy_println_float(double v);
    void ivy_println_str(const char* s);
    void ivy_println_char(char c);
}

int ivy_version();

#endif  // IVY_LIB_IVY_H
