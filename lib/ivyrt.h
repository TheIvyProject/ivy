// ivyc/lib/ivyrt.h — Ivy runtime library header.
//
// C5: Precompiled static library (libivyrt) containing the Ivy
// runtime.  User Ivy code never calls libc directly — it goes
// through these __ivy_* symbols, which are the only libc clients
// in the system.  The compiler (codegen) emits `declare` for the
// symbols it references and links libivyrt by default; the linker
// drops unused symbols from the static archive automatically.
//
//   - Allocator  : __ivy_alloc / __ivy_free   (wrap libc malloc/free)
//   - I/O         : __ivy_write_str / __ivy_write_char / __ivy_write_newline
//                   __ivy_fmt_int / __ivy_fmt_float / __ivy_fmt_bool
//   - Panic       : __ivy_panic
//
// Raw I/O uses the CRT _write primitive (POSIX write / Windows
// _write) — never printf — so the call site controls formatting.

#ifndef IVY_LIB_IVYRT_H
#define IVY_LIB_IVYRT_H

#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Allocator ─────────────────────────────────────────────────── */

/* Allocate `n` bytes.  Returns null on failure (like malloc). */
void* __ivy_alloc(unsigned long long n);

/* Free a pointer previously returned by __ivy_alloc.
 * Null is a no-op (unlike libc free). */
void __ivy_free(void* p);

/* ── Raw I/O ───────────────────────────────────────────────────── */

/* Write a NUL-terminated C string to file descriptor `fd`. */
void __ivy_write_str(int fd, const char* s);

/* Write a single character byte to `fd`. */
void __ivy_write_char(int fd, char ch);

/* Write a newline ('\n') to `fd`. */
void __ivy_write_newline(int fd);

/* ── Format helpers ────────────────────────────────────────────── */
/* Each returns a pointer to a NUL-terminated C string in a static
 * scratch buffer (overwritten on the next call).  Callers must
 * consume the result before formatting again.                   */

/* Format a signed 64-bit integer in decimal. */
char* __ivy_fmt_int(long long v);

/* Format a double-precision float. */
char* __ivy_fmt_float(double v);

/* Format a boolean as "true" or "false". */
char* __ivy_fmt_bool(_Bool b);

/* ── Panic ─────────────────────────────────────────────────────── */

/* Print `msg` to stdout and abort the process.  `line` is reserved
 * for future source-location diagnostics. */
void __ivy_panic(const char* msg, int line);

#ifdef __cplusplus
}
#endif

#endif  // IVY_LIB_IVYRT_H
