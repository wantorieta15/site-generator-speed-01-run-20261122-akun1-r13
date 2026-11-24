/* Runtime for the fused site-generator (round 4, Akun 1).
 * Freestanding (-nostdlib): raw syscalls only, no libc anywhere.
 *
 * Two-region arena over one mmap:
 *   RETAINED: grows monotonically within a case (JSON input, decoded
 *     strings, post data, page HTML, output buffer). Reset between cases.
 *   SCRATCH:  a separate region rewound to a mark after each page's
 *     temporaries (line state, template sub-buffers, tag refs). Keeping
 *     temporaries out of the retained region is what keeps the touched
 *     page count (and the minor-fault cost) low: the V6 bump allocator
 *     accumulated ~64 MB / ~16k faults over 3 cases.
 *
 * Buffers are raw cursors (char *p, *end): appends are plain memcpy with
 * one capacity check per record, not per byte/char. memcmp/memcpy use
 * 16-byte SSE2 chunks.
 */
#ifndef SG_RT_H
#define SG_RT_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef unsigned long usize;
typedef unsigned long uintptr_t;
typedef long isize;

/* ---- syscalls (x86-64) ---- */
static long sys0(long n) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory"); return r; }
static long sys1(long n, long a) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory"); return r; }
static long sys2(long n, long a, long b) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory"); return r; }
static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static long sys4(long n, long a, long b, long c, long d) {
  long r;
  __asm__ volatile("mov %5, %%r10; syscall"
                   : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(d) : "rcx", "r11", "r10", "memory");
  return r;
}
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  long r;
  __asm__ volatile("mov %5, %%r10; mov %6, %%r8; mov %7, %%r9; syscall"
                   : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(d), "r"(e), "r"(f) : "rcx", "r11", "r10", "r8", "r9", "memory");
  return r;
}

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPENAT 257
#define SYS_CLOSE 3
#define SYS_MMAP 9
#define SYS_EXIT 60
#define SYS_FSTAT 5
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

static void sys_exit(int code) { sys1(SYS_EXIT, code); __builtin_unreachable(); }
static void write_stderr(const char *p, usize n) { sys3(SYS_WRITE, 2, (long)p, (long)n); }

/* ---- memory helpers gcc expects (SSE2, 16-byte chunks) ---- */
#include <emmintrin.h>

void *memcpy(void *d, const void *s, usize n) {
  char *dd = (char *)d; const char *ss = (const char *)s;
  while (n >= 16) {
    _mm_storeu_si128((__m128i *)dd, _mm_loadu_si128((const __m128i *)ss));
    dd += 16; ss += 16; n -= 16;
  }
  while (n >= 8) { *(u64 *)dd = *(const u64 *)ss; dd += 8; ss += 8; n -= 8; }
  while (n--) *dd++ = *ss++;
  return d;
}
void *memset(void *d, int c, usize n) {
  char *dd = (char *)d;
  __m128i w = _mm_set1_epi8((char)c);
  while (n >= 16) { _mm_storeu_si128((__m128i *)dd, w); dd += 16; n -= 16; }
  while (n--) *dd++ = (char)c;
  return d;
}
void *memmove(void *d, const void *s, usize n) {
  char *dd = (char *)d; const char *ss = (const char *)s;
  if (dd <= ss) return memcpy(d, s, n);
  dd += n; ss += n;
  while (n >= 16) { dd -= 16; ss -= 16; _mm_storeu_si128((__m128i *)dd, _mm_loadu_si128((const __m128i *)ss)); n -= 16; }
  while (n--) *--dd = *--ss;
  return d;
}
/* memcmp: first differing byte decides; 0 when equal. SSE2, 16 bytes/step. */
int memcmp(const void *a, const void *b, usize n) {
  const u8 *x = (const u8 *)a, *y = (const u8 *)b;
  while (n >= 16) {
    __m128i vx = _mm_loadu_si128((const __m128i *)x);
    __m128i vy = _mm_loadu_si128((const __m128i *)y);
    int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(vx, vy));
    if (mask != 0xFFFF) {
      int bit = __builtin_ctz((unsigned)~(unsigned)mask);
      return (int)x[bit] - (int)y[bit];
    }
    x += 16; y += 16; n -= 16;
  }
  while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
  return 0;
}
/* memchr: first index of c in [p, p+n), or n. SSE2. */
static usize memchr_idx(const char *p, usize n, char c) {
  const __m128i cc = _mm_set1_epi8(c);
  usize i = 0;
  while (n - i >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(v, cc));
    if (mask) return i + (usize)__builtin_ctz((unsigned)mask);
    i += 16;
  }
  while (i < n) { if (p[i] == c) return i; i++; }
  return n;
}
usize strlen(const char *s) { usize n = 0; while (s[n]) n++; return n; }

/* ---- arenas ---- */
#define ARENA_SIZE (256ull << 20)
#define SCRATCH_SIZE (64ull << 20)
static char *arena_base, *arena_ptr, *arena_end;       /* retained */
static char *scratch_base, *scratch_ptr, *scratch_end; /* scratch */

static int arena_init(void) {
  long r = sys6(SYS_MMAP, 0, ARENA_SIZE + SCRATCH_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r < 0) return -1;
  arena_base = (char *)r; arena_ptr = arena_base; arena_end = arena_base + ARENA_SIZE;
  scratch_base = arena_base + ARENA_SIZE;
  scratch_ptr = scratch_base; scratch_end = scratch_base + SCRATCH_SIZE;
  return 0;
}
/* Retained allocation (lives until the next case reset). */
static void *ralloc(usize n) {
  n = (n + 15) & ~15ull;
  if (arena_ptr + n > arena_end) { write_stderr("arena out\n", 10); sys_exit(1); }
  void *p = arena_ptr; arena_ptr += n;
  return p;
}
/* Scratch allocation (rewound by the caller via marks). */
static void *salloc(usize n) {
  n = (n + 15) & ~15ull;
  if (scratch_ptr + n > scratch_end) { write_stderr("scratch out\n", 12); sys_exit(1); }
  void *p = scratch_ptr; scratch_ptr += n;
  return p;
}
static usize scratch_mark(void) { return (usize)(scratch_ptr - scratch_base); }
static void scratch_rewind(usize m) { scratch_ptr = scratch_base + m; }
static void case_reset(void) { arena_ptr = arena_base; scratch_ptr = scratch_base; }

/* ---- string slice ---- */
typedef struct { const char *p; u32 n; } Str;
static Str str_make(const char *p, u32 n) { Str s; s.p = p; s.n = n; return s; }
static Str str_c(const char *c) { return str_make(c, (u32)strlen(c)); }
static int str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
/* Code-unit order (JS sort). ASCII fast path is a plain memcmp (byte order
 * == code-unit order for ASCII); non-ASCII falls back to the decoder. */
static int str_cmp_cu(Str a, Str b);
static u64 hash_bytes(const char *p, u32 n) {
  u64 h = 14695981039346656037ull;
  for (u32 i = 0; i < n; i++) { h ^= (u8)p[i]; h *= 1099511628211ull; }
  return h;
}
static u64 hash_str(Str s) { return hash_bytes(s.p, s.n); }

/* ---- raw output cursor ---- */
typedef struct { char *p; char *cur; char *end; } Out;
static void out_init(Out *o, usize cap) {
  o->p = (char *)ralloc(cap); o->cur = o->p; o->end = o->p + cap;
}
static void out_need_slow(Out *o, usize extra) {
  usize used = (usize)(o->cur - o->p);
  usize ncap = (usize)(o->end - o->p) * 2;
  while (ncap < used + extra) ncap *= 2;
  char *np = (char *)ralloc(ncap);
  memcpy(np, o->p, used);
  o->p = np; o->cur = np + used; o->end = np + ncap;
}
/* V35 (ronde 10): the bounds check is inlined -- it runs on every
 * out_add/out_c (hundreds of thousands of dynamic calls; objdump showed
 * out_need as a real call at 99 sites, including the md_inline and
 * render_prog hot paths). The grow path stays a real function: it is
 * taken only a handful of times (buffers are pre-sized). */
static inline __attribute__((always_inline)) void out_need(Out *o, usize extra) {
  if ((usize)(o->end - o->cur) >= extra) return;
  out_need_slow(o, extra);
}
static inline __attribute__((always_inline)) void out_add(Out *o, const void *p, usize n) {
  out_need(o, n); memcpy(o->cur, p, n); o->cur += n;
}
static inline __attribute__((always_inline)) void out_adds(Out *o, Str s) { out_add(o, s.p, s.n); }
static inline __attribute__((always_inline)) void out_c(Out *o, char c) { out_need(o, 1); *o->cur++ = c; }
static inline __attribute__((always_inline)) void out_lit(Out *o, const char *lit) { out_add(o, lit, strlen(lit)); }
static Str out_str(Out *o) { return str_make(o->p, (u32)(o->cur - o->p)); }

/* A growable scratch buffer (reused across pages; lives in scratch). */
typedef struct { char *p; u32 n, cap; } Buf;
static void buf_init(Buf *b, u32 cap) { b->p = (char *)salloc(cap); b->n = 0; b->cap = cap; }
static void buf_need(Buf *b, u32 extra) {
  if (b->n + extra <= b->cap) return;
  u32 nc = b->cap * 2;
  while (nc < b->n + extra) nc *= 2;
  char *np = (char *)salloc(nc);
  memcpy(np, b->p, b->n);
  b->p = np; b->cap = nc;
}
/* Same shape, but retained (ralloc) so it survives scratch rewinds. */
typedef struct { char *p; u32 n, cap; } RBuf;
static void rbuf_init(RBuf *b, u32 cap) { b->p = (char *)ralloc(cap); b->n = 0; b->cap = cap; }
static void rbuf_need(RBuf *b, u32 extra) {
  if (b->n + extra <= b->cap) return;
  u32 nc = b->cap * 2;
  while (nc < b->n + extra) nc *= 2;
  char *np = (char *)ralloc(nc);
  memcpy(np, b->p, b->n);
  b->p = np; b->cap = nc;
}
static void rbuf_add(RBuf *b, const void *p, u32 n) { rbuf_need(b, n); memcpy(b->p + b->n, p, n); b->n += n; }
static void rbuf_addc(RBuf *b, char c) { rbuf_need(b, 1); b->p[b->n++] = c; }
static void rbuf_addlit(RBuf *b, const char *lit) { rbuf_add(b, lit, (u32)strlen(lit)); }
static void buf_add(Buf *b, const void *p, u32 n) { buf_need(b, n); memcpy(b->p + b->n, p, n); b->n += n; }
static void buf_adds(Buf *b, Str s) { buf_add(b, s.p, s.n); }
static void buf_addc(Buf *b, char c) { buf_need(b, 1); b->p[b->n++] = c; }
static void buf_addlit(Buf *b, const char *lit) { buf_add(b, lit, (u32)strlen(lit)); }
static Str buf_str(Buf *b) { return str_make(b->p, b->n); }

/* ---- small helpers ---- */
static int is_space_tab(char c) { return c == ' ' || c == '\t'; }
static int is_blank_line(const char *p, u32 n) {
  for (u32 i = 0; i < n; i++) if (!is_space_tab(p[i])) return 0;
  return 1;
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static int is_alnum_c(char c) { return is_lower(c) || is_upper(c) || is_digit(c); }

/* UTF-8 decode one code point; returns bytes consumed (1-4). */
static int utf8_decode(const char *p, u32 n, u32 *out) {
  if (n == 0) { *out = 0xFFFD; return 0; }
  u8 c = (u8)p[0];
  if (c < 0x80) { *out = c; return 1; }
  if ((c & 0xE0) == 0xC0 && n >= 2) { *out = ((c & 0x1F) << 6) | ((u8)p[1] & 0x3F); return 2; }
  if ((c & 0xF0) == 0xE0 && n >= 3) { *out = ((c & 0x0F) << 12) | (((u8)p[1] & 0x3F) << 6) | ((u8)p[2] & 0x3F); return 3; }
  if ((c & 0xF8) == 0xF0 && n >= 4) { *out = ((c & 0x07) << 18) | (((u8)p[1] & 0x3F) << 12) | (((u8)p[2] & 0x3F) << 6) | ((u8)p[3] & 0x3F); return 4; }
  *out = 0xFFFD; return 1;
}
static int utf8_encode(u32 cp, char *out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
/* All-ASCII test over a slice (8 bytes at a time). */
static int str_is_ascii(Str s) {
  const char *p = s.p; u32 n = s.n;
  while (n >= 8) { u64 w; memcpy(&w, p, 8); if (w & 0x8080808080808080ull) return 0; p += 8; n -= 8; }
  while (n--) if ((u8)*p++ >= 0x80) return 0;
  return 1;
}
static int str_cmp_cu(Str a, Str b) {
  if (str_is_ascii(a) && str_is_ascii(b)) {
    u32 n = a.n < b.n ? a.n : b.n;
    int c = n ? memcmp(a.p, b.p, n) : 0;
    if (c) return c;
    return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
  }
  u32 i = 0, j = 0;
  while (i < a.n && j < b.n) {
    u8 ca = (u8)a.p[i], cb = (u8)b.p[j];
    if (ca < 0x80 && cb < 0x80) {
      if (ca != cb) return ca < cb ? -1 : 1;
      i++; j++; continue;
    }
    u32 cpa, cpb;
    int na = utf8_decode(a.p + i, a.n - i, &cpa);
    int nb = utf8_decode(b.p + j, b.n - j, &cpb);
    u32 ua[2], ub[2]; int la = 1, lb = 1;
    if (cpa >= 0x10000) { ua[0] = 0xD800 + ((cpa - 0x10000) >> 10); ua[1] = 0xDC00 + ((cpa - 0x10000) & 0x3FF); la = 2; }
    else ua[0] = cpa;
    if (cpb >= 0x10000) { ub[0] = 0xD800 + ((cpb - 0x10000) >> 10); ub[1] = 0xDC00 + ((cpb - 0x10000) & 0x3FF); lb = 2; }
    else ub[0] = cpb;
    for (int k = 0; k < la && k < lb; k++) if (ua[k] != ub[k]) return ua[k] < ub[k] ? -1 : 1;
    if (la != lb) return la < lb ? -1 : 1;
    i += (u32)na; j += (u32)nb;
  }
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

#endif
