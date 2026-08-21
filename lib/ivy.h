// ivyc/lib/ivy.h — standard header for the Ivy subset.
//
// This is intentionally minimal: it provides declarations that Ivy programs
// can pull in via `#include <ivy.h>`. It exists primarily to exercise the
// preprocessor's angle-bracket search path (-I).

#ifndef IVY_LIB_IVY_H
#define IVY_LIB_IVY_H

extern "C" void* malloc(unsigned long n);
extern "C" void free(void* p);

int ivy_version();

#endif  // IVY_LIB_IVY_H
