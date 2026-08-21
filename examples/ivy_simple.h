// ivyc/examples/ivy_simple.h — minimal header without include guards.
// Used to verify that #include expansion feeds the rest of the pipeline
// (parser -> HIR -> MIR -> LLVM IR) without tripping the unsupported-
// directive path (no #ifndef / #define / #endif yet).

extern "C" void* malloc(unsigned long n);
extern "C" void free(void* p);
