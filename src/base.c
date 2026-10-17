/* base.c - implementation of base.h */
#include "base.h"

#if FS_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

void fs_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fs_fatal("out of memory (%zu bytes)", n);
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) fs_fatal("out of memory (%zu x %zu bytes)", count, size);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) fs_fatal("out of memory (%zu bytes)", n);
    return q;
}

char *xstrndup(const char *s, size_t n) {
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

/* ---- arena --------------------------------------------------------- */
struct ArenaBlock {
    ArenaBlock *next;
    size_t used, cap;
    /* data follows, 16-byte aligned */
};
#define ARENA_HDR ((sizeof(ArenaBlock) + 15) & ~(size_t)15)

void *arena_alloc(Arena *a, size_t size) {
    size = (size + 15) & ~(size_t)15;
    ArenaBlock *b = a->head;
    if (!b || b->used + size > b->cap) {
        size_t cap = size > 65536 ? size : 65536;
        b = (ArenaBlock *)xmalloc(ARENA_HDR + cap);
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    void *p = (char *)b + ARENA_HDR + b->used;
    b->used += size;
    memset(p, 0, size);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *arena_printf(Arena *a, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = (char *)arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
}

/* ---- vectors ------------------------------------------------------- */
void vec_grow_(void **data, int *cap, int need, size_t elem) {
    if (need <= *cap) return;
    int c = *cap ? *cap * 2 : 8;
    while (c < need) c *= 2;
    *data = xrealloc(*data, (size_t)c * elem);
    *cap = c;
}

/* ---- string builder ------------------------------------------------ */
static void sb_reserve(StrBuf *sb, int extra) {
    int need = sb->len + extra + 1;
    if (need <= sb->cap) return;
    int c = sb->cap ? sb->cap * 2 : 64;
    while (c < need) c *= 2;
    sb->data = (char *)xrealloc(sb->data, (size_t)c);
    sb->cap = c;
}

void sb_append(StrBuf *sb, const char *s, int len) {
    sb_reserve(sb, len);
    memcpy(sb->data + sb->len, s, (size_t)len);
    sb->len += len;
    sb->data[sb->len] = 0;
}

void sb_puts(StrBuf *sb, const char *s) { sb_append(sb, s, (int)strlen(s)); }

void sb_putc(StrBuf *sb, char c) { sb_append(sb, &c, 1); }

void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return;
    sb_reserve(sb, n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    sb->len += n;
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(sb, fmt, ap);
    va_end(ap);
}

void sb_clear(StrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = 0;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

const char *sb_cstr(StrBuf *sb) { return sb->data ? sb->data : ""; }

/* ---- files --------------------------------------------------------- */
char *fs_read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    StrBuf sb = {0};
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_append(&sb, buf, (int)n);
    fclose(f);
    if (!sb.data) sb_append(&sb, "", 0);
    if (len_out) *len_out = (size_t)sb.len;
    return sb.data;
}

bool fs_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

const char *fs_basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* ---- UTF-8 --------------------------------------------------------- */
int utf8_decode(const char *str, int len, u32 *cp) {
    const u8 *s = (const u8 *)str;
    if (len <= 0) {
        *cp = 0;
        return 0;
    }
    u8 c = s[0];
    int n;
    u32 v;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2;
        v = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        v = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        v = c & 0x07;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if (n > len) {
        *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (s[i] & 0x3F);
    }
    /* reject overlong encodings and surrogates */
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && v < 0x10000) || v > 0x10FFFF ||
        (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = v;
    return n;
}

int utf8_encode(u32 cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return utf8_encode(0xFFFD, out);
}

/* ---- misc ---------------------------------------------------------- */
double fs_time_seconds(void) {
#if FS_OS_WINDOWS
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

u64 fs_hash_bytes(const void *p, size_t n) {
    const u8 *s = (const u8 *)p;
    u64 h = 1469598103934665603ull; /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}
