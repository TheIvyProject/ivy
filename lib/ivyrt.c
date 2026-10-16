/* ivy/lib/ivyrt.c — Ivy runtime library (C bootstrap).
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
#include <stdio.h>    /* puts, snprintf, fgets, fopen, fclose, fread, fwrite (file I/O) */
#include <math.h>     /* sqrt, sin, cos, pow, floor, ceil, etc. (D7 math wrappers) */
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

void __ivy_memset(void* dst, int c, unsigned long long n) {
    char* d = (char*)dst;
    char ch = (char)c;
    for (unsigned long long i = 0; i < n; ++i) {
        d[i] = ch;
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

/* ── Stdin ────────────────────────────────────────────────────── */

char* __ivy_read_line(void) {
    /* Read a line from stdin using fgets.  Returns an allocated
     * NUL-terminated string (caller must free).  Strips trailing
     * newline.  Returns null on EOF with no data. */
    char buf[1024];
    char* result = (char*)0;
    unsigned long long resultLen = 0;

    while (1) {
        char* r = fgets(buf, (int)sizeof(buf), stdin);
        if (!r) {
            /* EOF or error.  If we have accumulated data, return it. */
            return result;  /* may be null */
        }
        unsigned long long chunkLen = strlen(buf);
        /* Check for newline — line complete. */
        int hasNewline = 0;
        if (chunkLen > 0 && buf[chunkLen - 1] == '\n') {
            hasNewline = 1;
            chunkLen--;  /* exclude newline */
        }

        /* Append to result. */
        unsigned long long newLen = resultLen + chunkLen;
        char* newBuf = (char*)__ivy_alloc(newLen + 1);
        if (!newBuf) {
            __ivy_free(result);
            return 0;
        }
        if (result) {
            __ivy_memcpy(newBuf, result, resultLen);
        }
        __ivy_memcpy(newBuf + resultLen, buf, chunkLen);
        newBuf[newLen] = '\0';
        __ivy_free(result);
        result = newBuf;
        resultLen = newLen;

        if (hasNewline) {
            return result;
        }
        /* No newline — line was longer than buffer, loop to read more. */
    }
}

/* ── File I/O ──────────────────────────────────────────────────── */

/* Opaque file handle wraps FILE*. */
struct __ivy_file {
    /* Internally, this IS a FILE*.  We store the FILE pointer
     * directly as the first member so that `(__ivy_file*)f` can
     * be cast to `(FILE*)f`.  This avoids exposing <stdio.h>
     * to Ivy code. */
    void* handle;   /* Actually FILE* */
};

/* The struct __ivy_file is designed so that its address IS the
 * FILE* address.  We use a simple trick: allocate a struct whose
 * first member is a FILE*. */
#define IVY_FILE_PTR(f)  ((FILE*)((f)->handle))

__ivy_file* __ivy_file_open_read(const char* path) {
    if (!path) return 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    __ivy_file* f = (__ivy_file*)__ivy_alloc(sizeof(__ivy_file));
    if (!f) { fclose(fp); return 0; }
    f->handle = (void*)fp;
    return f;
}

__ivy_file* __ivy_file_open_write(const char* path) {
    if (!path) return 0;
    FILE* fp = fopen(path, "wb");
    if (!fp) return 0;
    __ivy_file* f = (__ivy_file*)__ivy_alloc(sizeof(__ivy_file));
    if (!f) { fclose(fp); return 0; }
    f->handle = (void*)fp;
    return f;
}

void __ivy_file_close(__ivy_file* f) {
    if (!f) return;
    if (f->handle) {
        fclose(IVY_FILE_PTR(f));
    }
    __ivy_free(f);
}

unsigned long long __ivy_file_read(__ivy_file* f, void* buf,
                                     unsigned long long n) {
    if (!f || !f->handle || !buf || n == 0) return 0;
    size_t got = fread(buf, 1, (size_t)n, IVY_FILE_PTR(f));
    return (unsigned long long)got;
}

unsigned long long __ivy_file_write(__ivy_file* f, const void* buf,
                                      unsigned long long n) {
    if (!f || !f->handle || !buf || n == 0) return 0;
    size_t put = fwrite(buf, 1, (size_t)n, IVY_FILE_PTR(f));
    return (unsigned long long)put;
}

char* __ivy_read_file(const char* path, unsigned long long* outLen) {
    if (outLen) *outLen = 0;
    if (!path) return 0;
    __ivy_file* f = __ivy_file_open_read(path);
    if (!f) return 0;

    /* Read in chunks, growing the buffer. */
    unsigned long long cap = 4096;
    unsigned long long len = 0;
    char* buf = (char*)__ivy_alloc(cap);
    if (!buf) { __ivy_file_close(f); return 0; }

    while (1) {
        if (len + 4096 > cap) {
            unsigned long long newCap = cap * 2;
            char* newBuf = (char*)__ivy_alloc(newCap);
            if (!newBuf) {
                __ivy_free(buf);
                __ivy_file_close(f);
                return 0;
            }
            __ivy_memcpy(newBuf, buf, len);
            __ivy_free(buf);
            buf = newBuf;
            cap = newCap;
        }
        unsigned long long got = __ivy_file_read(f, buf + len, 4096);
        if (got == 0) break;
        len += got;
    }
    __ivy_file_close(f);

    /* NUL-terminate for convenience. */
    if (len >= cap) {
        char* newBuf = (char*)__ivy_alloc(len + 1);
        if (!newBuf) { __ivy_free(buf); return 0; }
        __ivy_memcpy(newBuf, buf, len);
        __ivy_free(buf);
        buf = newBuf;
    }
    buf[len] = '\0';
    if (outLen) *outLen = len;
    return buf;
}

int __ivy_write_file(const char* path, const void* buf,
                      unsigned long long n) {
    if (!path || !buf) return 1;
    __ivy_file* f = __ivy_file_open_write(path);
    if (!f) return 1;
    unsigned long long wrote = __ivy_file_write(f, buf, n);
    __ivy_file_close(f);
    return (wrote == n) ? 0 : 1;
}

/* ── Math wrappers (D7) ────────────────────────────────────────
 * Thin wrappers around libc math functions.  Ivy modules call these
 * via `extern "C"` declarations; the Ivy wrapper functions (e.g.
 * `export double sqrt(double x)`) forward to these. */

double __ivy_sqrt(double x)   { return sqrt(x); }
double __ivy_sin(double x)    { return sin(x); }
double __ivy_cos(double x)    { return cos(x); }
double __ivy_tan(double x)    { return tan(x); }
double __ivy_pow(double b, double e) { return pow(b, e); }
double __ivy_floor(double x) { return floor(x); }
double __ivy_ceil(double x)  { return ceil(x); }
double __ivy_round(double x) { return round(x); }
double __ivy_trunc(double x) { return trunc(x); }
double __ivy_exp(double x)   { return exp(x); }
double __ivy_log(double x)   { return log(x); }
double __ivy_log2(double x)  { return log2(x); }
double __ivy_log10(double x) { return log10(x); }
double __ivy_atan(double x)  { return atan(x); }
double __ivy_asin(double x)  { return asin(x); }
double __ivy_acos(double x)  { return acos(x); }
double __ivy_atan2(double y, double x) { return atan2(y, x); }
double __ivy_fmod(double a, double b) { return fmod(a, b); }
double __ivy_fabs(double x) { return fabs(x); }
