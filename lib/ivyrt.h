// ivyc/lib/ivyrt.h — Ivy runtime library header.
//
// C2: Ivy-safe allocator API.  User code never calls malloc/free
// directly — it goes through __ivy_alloc / __ivy_free, which wrap
// libc malloc/free internally.  This indirection allows:
//
//   - Safety checks (null, overflow, tracking)
//   - Future arena layer on top (bump alloc for temporaries)
//   - Debug-mode instrumentation (leak detection, use-after-free)
//
// The implementations are emitted inline by codegen (like __ivy_panic)
// so no external libivyrt is needed for the current bootstrap phase.

#ifndef IVY_LIB_IVYRT_H
#define IVY_LIB_IVYRT_H

#ifdef __cplusplus
extern "C" {
#endif

// Allocate `n` bytes.  Returns null on failure (like malloc).
// Currently: thin wrapper over libc malloc.
void* __ivy_alloc(unsigned long n);

// Free a pointer previously returned by __ivy_alloc.
// Null is a no-op (unlike libc free).
void __ivy_free(void* p);

#ifdef __cplusplus
}
#endif

#endif  // IVY_LIB_IVYRT_H
