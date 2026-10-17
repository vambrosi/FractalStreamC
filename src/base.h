/* base.h - common definitions shared by every module (unity build).
 *
 * Conventions:
 *   - Every module is a .c file included once by src/all.c.  Headers only
 *     declare the public interface of a module so other modules (and the
 *     tests) know what they may use.
 *   - Prefix public functions with the module name (lex_, parse_, gfx_, ...).
 *   - Module-private helpers are `static`.
 *   - Memory: arenas for compiler data (freed in one go), xmalloc/free for
 *     long-lived runtime data.  Out-of-memory aborts.
 */
#ifndef FS_BASE_H
#define FS_BASE_H

#if defined(_MSC_VER) && !defined(_USE_MATH_DEFINES)
#define _USE_MATH_DEFINES /* M_PI */
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <float.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

#define FS_VERSION "0.1.0"

#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define FS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define FS_MAX(a, b) ((a) > (b) ? (a) : (b))
#define FS_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

#if defined(__MINGW32__)
#define FS_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#define FS_NORETURN __attribute__((noreturn))
#elif defined(__GNUC__) || defined(__clang__)
#define FS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define FS_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define FS_PRINTF(f, a)
#define FS_NORETURN __declspec(noreturn)
#else
#define FS_PRINTF(f, a)
#define FS_NORETURN
#endif

#if defined(_WIN32)
#define FS_OS_WINDOWS 1
#elif defined(__APPLE__)
#define FS_OS_MACOS 1
#else
#define FS_OS_LINUX 1
#endif

/* ---- errors -------------------------------------------------------- */
FS_NORETURN void fs_fatal(const char *fmt, ...) FS_PRINTF(1, 2);
#define FS_ASSERT(c) ((c) ? (void)0 : fs_fatal("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__))

/* ---- heap ---------------------------------------------------------- */
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- arena --------------------------------------------------------- */
typedef struct ArenaBlock ArenaBlock;
typedef struct Arena {
    ArenaBlock *head;
} Arena;
void *arena_alloc(Arena *a, size_t size); /* zeroed, 16-byte aligned */
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_printf(Arena *a, const char *fmt, ...) FS_PRINTF(2, 3);
void arena_free(Arena *a);
#define ARENA_NEW(a, T) ((T *)arena_alloc((a), sizeof(T)))
#define ARENA_ARRAY(a, T, n) ((T *)arena_alloc((a), sizeof(T) * (size_t)(n)))

/* ---- dynamic arrays -----------------------------------------------
 * Declare with a typedef:  typedef VEC(int) IntVec;  IntVec v = {0};
 * vec_push(v, 3); v.data[i]; v.len; vec_free(v);                     */
#define VEC(T) struct { T *data; int len, cap; }
void vec_grow_(void **data, int *cap, int need, size_t elem);
#define vec_reserve(v, n) vec_grow_((void **)&(v).data, &(v).cap, (n), sizeof(*(v).data))
#define vec_push(v, x) (vec_reserve((v), (v).len + 1), (v).data[(v).len++] = (x))
#define vec_pop(v) ((v).data[--(v).len])
#define vec_last(v) ((v).data[(v).len - 1])
#define vec_clear(v) ((v).len = 0)
#define vec_free(v) (free((v).data), (v).data = NULL, (v).len = (v).cap = 0)

/* ---- string builder ------------------------------------------------ */
typedef struct StrBuf {
    char *data; /* always NUL-terminated when non-NULL */
    int len, cap;
} StrBuf;
void sb_append(StrBuf *sb, const char *s, int len);
void sb_puts(StrBuf *sb, const char *s);
void sb_putc(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...) FS_PRINTF(2, 3);
void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap);
void sb_clear(StrBuf *sb);
void sb_free(StrBuf *sb);
const char *sb_cstr(StrBuf *sb); /* "" if empty */

/* ---- files --------------------------------------------------------- */
char *fs_read_file(const char *path, size_t *len_out); /* NUL-terminated, NULL on failure */
bool fs_write_file(const char *path, const void *data, size_t len);
const char *fs_basename(const char *path);

/* ---- UTF-8 ---------------------------------------------------------
 * utf8_decode returns the byte length of the sequence at s (>=1 when
 * len>0); invalid bytes decode as U+FFFD with length 1.               */
int utf8_decode(const char *s, int len, u32 *cp);
int utf8_encode(u32 cp, char out[4]);

/* ---- misc ---------------------------------------------------------- */
double fs_time_seconds(void); /* monotonic */
u64 fs_hash_bytes(const void *p, size_t n);

#endif
