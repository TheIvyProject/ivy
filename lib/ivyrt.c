/* ivyc/lib/ivyrt.c — Ivy runtime library (C bootstrap).
 *
 * C5: Precompiled static library containing the Ivy runtime:
 *   - Allocator  (__ivy_alloc / __ivy_free  — wrap libc malloc/free)
 *   - I/O         (__ivy_write_str / __ivy_write_char / __ivy_write_newline
 *                 __ivy_fmt_int / __ivy_fmt_float / __ivy_fmt_bool)
 *   - Panic       (__ivy_panic)
 *
 * User Ivy code never calls libc directly — it goes through these
 * __ivy_* symbols, which are the only libc clients in the system.
 * The compiler (codegen) emits `declare` for these and links
 * libivyrt by default; the linker drops unused symbols from the
 * static archive automatically.
 *
 * Raw I/O uses the CRT `_write` primitive (POSIX write / Windows
 * _write) — never printf — so the call site controls formatting.
 */

#include "ivyrt.h"

#include <stdlib.h>   /* malloc, free, abort */
#include <string.h>   /* strlen */
#include <stdio.h>    /* puts, snprintf (inside __ivy_fmt_int only) */
#ifdef _WIN32
#  include <io.h>     /* _write, _gcvt */
#else
#  include <unistd.h> /* write */
#  define _write(fd, buf, n)  write((fd), (buf), (int)(n))
#endif

/* ── Allocator ─────────────────────────────────────────────────── */

void* __ivy_alloc(unsigned long long n) {
    return malloc(n);
}

void __ivy_free(void* p) {
    if (!p) return;             /* null-safe, unlike libc free */
    free(p);
}

/* ── Raw I/O ───────────────────────────────────────────────────── */

void __ivy_write_str(int fd, const char* s) {
    if (!s) return;
    unsigned long long len = strlen(s);
    _write(fd, s, (int)len);
}

void __ivy_write_char(int fd, char ch) {
    _write(fd, &ch, 1);
}

void __ivy_write_newline(int fd) {
    char nl = '\n';
    _write(fd, &nl, 1);
}

/* ── Format helpers ────────────────────────────────────────────── */
/* Each returns a pointer to a NUL-terminated C string in a static
 * scratch buffer.  The buffer is overwritten on the next call, so
 * callers should consume the result before formatting again.      */

char* __ivy_fmt_int(long long v) {
    static char buf[32];
    snprintf(buf, sizeof buf, "%lld", v);
    return buf;
}

char* __ivy_fmt_float(double v) {
    static char buf[64];
#ifdef _WIN32
    /* _gcvt is non-variadic so it avoids the Windows x64 varargs
     * double ABI issue seen with snprintf("%g", dbl) under LLJIT.
     * 15 significant digits is enough for IEEE-754 double. */
    _gcvt(v, 15, buf);
#else
    snprintf(buf, sizeof buf, "%.6g", v);
#endif
    return buf;
}

char* __ivy_fmt_bool(_Bool b) {
    return b ? "true" : "false";
}

/* ── Panic ─────────────────────────────────────────────────────── */

void __ivy_panic(const char* msg, int line) {
    if (msg) {
        puts(msg);
    }
    (void)line;                 /* reserved for future diagnostics */
    abort();
}

/* ── String operations ─────────────────────────────────────────── */

unsigned long long __ivy_strlen(const char* s) {
    if (!s) return 0;
    return strlen(s);
}

void __ivy_memcpy(void* dst, const void* src, unsigned long long n) {
    /* We use a simple byte loop rather than calling libc memcpy to
     * keep the dependency surface minimal.  The optimiser will
     * vectorise this for large `n`. */
    char* d = (char*)dst;
    const char* s = (const char*)src;
    for (unsigned long long i = 0; i < n; ++i) {
        d[i] = s[i];
    }
}

int __ivy_strcmp(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    /* libc strcmp semantics */
    while (*a && (*a == *b)) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char* __ivy_strdup(const char* s) {
    if (!s) return 0;
    unsigned long long len = strlen(s);
    char* buf = (char*)__ivy_alloc(len + 1);
    if (!buf) return 0;
    __ivy_memcpy(buf, s, len + 1);   /* include NUL */
    return buf;
}
