/* Streaming JSON for cases.json. No value tree: the scanner walks the
 * input once and extracts exactly the fields the build needs (site,
 * templates, files) straight into the Input structs.
 *
 * Strings: zero-copy slices when they hold no backslash; otherwise decoded
 * with a SIMD scan (find the next " or \ 16 bytes at a time, memcpy the
 * clean runs) into the retained arena. Escape semantics mirror the oracle
 * exactly: \" \\ \/ \b \f \n \r \t, \uXXXX with surrogate-pair combining,
 * and a lone surrogate kept as the literal 6 bytes \uXXXX (uppercase hex).
 * Numbers are doubles (only per_page/recent are read, truncated to long).
 */
#ifndef SG_JSON_H
#define SG_JSON_H

#include "rt.h"
#include "text.h"

typedef struct { Str path, content; } InFile;
typedef struct { Str name, src; } InTpl;
typedef struct {
  Str title, base_url;
  long per_page, recent;
} InSite;
typedef struct {
  InSite site;
  InTpl *tpls; u32 ntpls;
  InFile *files; u32 nfiles;
} Input;

typedef struct { const char *p; const char *end; int err; RBuf dec; } JScan;

static void j_skip_ws(JScan *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

/* SIMD: next index holding " or \, and which one (0=quote, 1=backslash). */
static __m128i JS_Q, JS_BS;
static int JS_V_OK = 0;
static void json_init(void) {
  if (JS_V_OK) return;
  JS_Q = _mm_set1_epi8('"'); JS_BS = _mm_set1_epi8('\\');
  JS_V_OK = 1;
}
#if defined(__x86_64__)
static const char *find_quote_or_bs_sse2(const char *p, const char *end, int *is_bs);
/* V27: AVX2 32B scan (runtime dispatch; falls back to SSE2). The pragma
 * enables AVX2 only for this function; the rest of the TU stays
 * x86-64-v2 so the binary still runs on non-AVX2 CPUs. */
#pragma GCC push_options
#pragma GCC target("avx2")
#include <immintrin.h>
static const char *find_quote_or_bs_avx2(const char *p, const char *end, int *is_bs) {
  __m256i q = _mm256_set1_epi8('"'), bs = _mm256_set1_epi8('\\');
  while (end - p >= 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs));
    int mask = _mm256_movemask_epi8(m);
    if (mask) {
      int bit = __builtin_ctz((unsigned)mask);
      *is_bs = (p[bit] == '\\');
      return p + bit;
    }
    p += 32;
  }
  /* Tail (< 32 bytes): call the SSE2 body DIRECTLY — calling the
   * dispatching find_quote_or_bs here would recurse forever. */
  return find_quote_or_bs_sse2(p, end, is_bs);
}
#pragma GCC pop_options
static int cpu_has_avx2(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  u32 eax, ebx, ecx, edx;
  __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
  int avx2 = (ebx >> 5) & 1;
  __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
  int osxsave = (ecx >> 27) & 1;
  int ok = 0;
  if (avx2 && osxsave) {
    u32 lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    ok = ((lo & 0x6) == 0x6);
  }
  cached = ok;
  return ok;
}
#endif
static const char *find_quote_or_bs_sse2(const char *p, const char *end, int *is_bs) {
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, JS_Q), _mm_cmpeq_epi8(v, JS_BS));
    int mask = _mm_movemask_epi8(m);
    if (mask) {
      int bit = __builtin_ctz((unsigned)mask);
      *is_bs = (p[bit] == '\\');
      return p + bit;
    }
    p += 16;
  }
  while (p < end) {
    if (*p == '"') { *is_bs = 0; return p; }
    if (*p == '\\') { *is_bs = 1; return p; }
    p++;
  }
  *is_bs = 0;
  return end;
}
/* Dispatcher: AVX2 32B scan when the CPU supports it, else SSE2. */
static const char *find_quote_or_bs(const char *p, const char *end, int *is_bs) {
#if defined(__x86_64__)
  if (cpu_has_avx2()) return find_quote_or_bs_avx2(p, end, is_bs);
#endif
  return find_quote_or_bs_sse2(p, end, is_bs);
}

static int j_hex4(const char *p) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i]; v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

/* Parse one string starting at the opening quote. */
static Str j_string(JScan *j) {
  j->p++; /* opening quote */
  const char *start = j->p;
  int is_bs = 0;
  const char *hit = find_quote_or_bs(j->p, j->end, &is_bs);
  if (hit >= j->end) { j->err = 1; return str_make(0, 0); }
  if (!is_bs) {
    Str s = str_make(start, (u32)(hit - start));
    j->p = hit + 1;
    return s;
  }
  /* Escapes present: decode IN PLACE. The decoded output is never
   * longer than the raw span (every escape shrinks or keeps length),
   * and the write cursor never passes the read cursor, so bytes ahead
   * -- including the closing quote -- are untouched and the parser's
   * forward scan is unaffected. The input buffer lives for the whole
   * run, so the slice stays valid with no extra copy and no RBuf. */
  char *w = (char *)start;
  const char *p = start;
  for (;;) {
    if (hit > p) { usize n = (usize)(hit - p); memcpy(w, p, n); w += n; }
    /* hit points at a backslash (or the closing quote). */
    if (*hit == '"') { j->p = hit + 1; break; }
    /* escape at hit[0]=='\\' */
    if (hit + 1 >= j->end) { j->err = 1; break; }
    char e = hit[1];
    if (e == 'u' && hit + 5 < j->end) {
      int cp = j_hex4(hit + 2);
      if (cp < 0) { j->err = 1; break; }
      const char *np = hit + 6;
      if (cp >= 0xD800 && cp <= 0xDBFF && np + 6 <= j->end && np[0] == '\\' && np[1] == 'u') {
        int lo = j_hex4(np + 2);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          u32 full = 0x10000 + ((u32)(cp - 0xD800) << 10) + (u32)(lo - 0xDC00);
          char tmp[4]; int n = utf8_encode(full, tmp);
          memcpy(w, tmp, (u32)n); w += (u32)n;
          p = np + 6;
          hit = find_quote_or_bs(p, j->end, &is_bs);
          if (hit >= j->end) { j->err = 1; break; }
          if (!is_bs) { if (hit > p) { usize n2 = (usize)(hit - p); memcpy(w, p, n2); w += n2; } j->p = hit + 1; break; }
          continue;
        }
      }
      if (cp >= 0xD800 && cp <= 0xDFFF) {
        memcpy(w, "\\u", 2); w += 2;
        char hx[4]; const char *H = "0123456789ABCDEF";
        hx[0] = H[(cp >> 12) & 15]; hx[1] = H[(cp >> 8) & 15]; hx[2] = H[(cp >> 4) & 15]; hx[3] = H[cp & 15];
        memcpy(w, hx, 4); w += 4;
        p = np;
        hit = find_quote_or_bs(p, j->end, &is_bs);
        if (hit >= j->end) { j->err = 1; break; }
        if (!is_bs) { if (hit > p) { usize n2 = (usize)(hit - p); memcpy(w, p, n2); w += n2; } j->p = hit + 1; break; }
        continue;
      }
      char tmp[4]; int n = utf8_encode((u32)cp, tmp);
      memcpy(w, tmp, (u32)n); w += (u32)n;
      p = np;
      hit = find_quote_or_bs(p, j->end, &is_bs);
      if (hit >= j->end) { j->err = 1; break; }
      if (!is_bs) { if (hit > p) { usize n2 = (usize)(hit - p); memcpy(w, p, n2); w += n2; } j->p = hit + 1; break; }
      continue;
    }
    switch (e) {
      case '"': *w++ = '"'; break;
      case '\\': *w++ = '\\'; break;
      case '/': *w++ = '/'; break;
      case 'b': *w++ = '\b'; break;
      case 'f': *w++ = '\f'; break;
      case 'n': *w++ = '\n'; break;
      case 'r': *w++ = '\r'; break;
      case 't': *w++ = '\t'; break;
      default: j->err = 1; break;
    }
    if (j->err) break;
    p = hit + 2;
    hit = find_quote_or_bs(p, j->end, &is_bs);
    if (hit >= j->end) { j->err = 1; break; }
    if (!is_bs) { if (hit > p) { usize n2 = (usize)(hit - p); memcpy(w, p, n2); w += n2; } j->p = hit + 1; break; }
  }
  if (j->err) return str_make(0, 0);
  return str_make(start, (u32)(w - start));
}

static double j_number(JScan *j) {
  int neg = 0;
  if (j->p < j->end && *j->p == '-') { neg = 1; j->p++; }
  u64 mant = 0; int ndig = 0;
  while (j->p < j->end && is_digit(*j->p)) { if (ndig < 19) mant = mant * 10 + (u64)(*j->p - '0'); ndig++; j->p++; }
  double v = (double)mant;
  int exp10 = 0;
  if (j->p < j->end && *j->p == '.') {
    j->p++;
    while (j->p < j->end && is_digit(*j->p)) { if (ndig < 19) { mant = mant * 10 + (u64)(*j->p - '0'); ndig++; } exp10--; j->p++; }
    v = (double)mant;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    j->p++;
    int eneg = 0; if (j->p < j->end && *j->p == '-') { eneg = 1; j->p++; } else if (j->p < j->end && *j->p == '+') j->p++;
    int e = 0; while (j->p < j->end && is_digit(*j->p)) { e = e * 10 + (*j->p - '0'); j->p++; }
    exp10 += eneg ? -e : e;
  }
  static const double P10[] = {1, 10, 100, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  if (exp10 >= 0) { while (exp10 >= 22) { v *= 1e22; exp10 -= 22; } v *= P10[exp10]; }
  else { exp10 = -exp10; while (exp10 >= 22) { v /= 1e22; exp10 -= 22; } v /= P10[exp10]; }
  return neg ? -v : v;
}

/* Skip one JSON value (object, array, string, number, literal). */
static void j_skip_value(JScan *j) {
  j_skip_ws(j);
  if (j->p >= j->end) { j->err = 1; return; }
  char c = *j->p;
  if (c == '"') { (void)j_string(j); return; }
  if (c == '{') {
    j->p++;
    j_skip_ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return; }
    for (;;) {
      j_skip_ws(j);
      if (j->p >= j->end || *j->p != '"') { j->err = 1; return; }
      (void)j_string(j);
      if (j->err) return;
      j_skip_ws(j);
      if (j->p >= j->end || *j->p != ':') { j->err = 1; return; }
      j->p++;
      j_skip_value(j);
      if (j->err) return;
      j_skip_ws(j);
      if (j->p < j->end && *j->p == ',') { j->p++; continue; }
      if (j->p < j->end && *j->p == '}') { j->p++; return; }
      j->err = 1; return;
    }
  }
  if (c == '[') {
    j->p++;
    j_skip_ws(j);
    if (j->p < j->end && *j->p == ']') { j->p++; return; }
    for (;;) {
      j_skip_value(j);
      if (j->err) return;
      j_skip_ws(j);
      if (j->p < j->end && *j->p == ',') { j->p++; continue; }
      if (j->p < j->end && *j->p == ']') { j->p++; return; }
      j->err = 1; return;
    }
  }
  if (c == 't') { if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) { j->p += 4; return; } j->err = 1; return; }
  if (c == 'f') { if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) { j->p += 5; return; } j->err = 1; return; }
  if (c == 'n') { if (j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) { j->p += 4; return; } j->err = 1; return; }
  if (c == '-' || is_digit(c)) { (void)j_number(j); return; }
  j->err = 1;
}

static int j_expect(JScan *j, char c) {
  j_skip_ws(j);
  if (j->p < j->end && *j->p == c) { j->p++; return 1; }
  j->err = 1;
  return 0;
}

/* ---- the cases.json walk ----
 * Calls `on_case(in, ctx)` once per case with a filled Input. Returns 0
 * on malformed input. The Input's arrays/strings live in the retained
 * arena; the caller resets the case arena between cases.
 */
typedef void (*CaseFn)(Input *in, void *ctx);

static int parse_cases(const char *data, u32 ndata, CaseFn on_case, void *ctx) {
  JScan j; j.p = data; j.end = data + ndata; j.err = 0;
  rbuf_init(&j.dec, 8192);
  json_init();
  text_init();
  if (!j_expect(&j, '[')) return 0;
  j_skip_ws(&j);
  if (j.p < j.end && *j.p == ']') { j.p++; return 1; }
  for (;;) {
    /* One case object: { "id": ..., "input": {...} } */
    if (!j_expect(&j, '{')) return 0;
    Input in;
    in.site.title = str_make(0, 0); in.site.base_url = str_make(0, 0);
    in.site.per_page = 0; in.site.recent = 0;
    in.tpls = 0; in.ntpls = 0; in.files = 0; in.nfiles = 0;
    u32 tcap = 0, fcap = 0;
    j_skip_ws(&j);
    if (j.p < j.end && *j.p == '}') { j.p++; }
    else for (;;) {
      j_skip_ws(&j);
      if (j.p >= j.end || *j.p != '"') return 0;
      Str key = j_string(&j);
      if (j.err) return 0;
      if (!j_expect(&j, ':')) return 0;
      if (str_eq(key, str_c("input"))) {
        /* The input object. */
        if (!j_expect(&j, '{')) return 0;
        j_skip_ws(&j);
        if (j.p < j.end && *j.p == '}') { j.p++; }
        else for (;;) {
          j_skip_ws(&j);
          if (j.p >= j.end || *j.p != '"') return 0;
          Str ikey = j_string(&j);
          if (j.err) return 0;
          if (!j_expect(&j, ':')) return 0;
          if (str_eq(ikey, str_c("site"))) {
            if (!j_expect(&j, '{')) return 0;
            j_skip_ws(&j);
            if (j.p < j.end && *j.p == '}') { j.p++; }
            else for (;;) {
              j_skip_ws(&j);
              if (j.p >= j.end || *j.p != '"') return 0;
              Str skey = j_string(&j);
              if (j.err) return 0;
              if (!j_expect(&j, ':')) return 0;
              j_skip_ws(&j);
              if (str_eq(skey, str_c("title"))) in.site.title = j_string(&j);
              else if (str_eq(skey, str_c("base_url"))) in.site.base_url = j_string(&j);
              else if (str_eq(skey, str_c("per_page"))) in.site.per_page = (long)j_number(&j);
              else if (str_eq(skey, str_c("recent"))) in.site.recent = (long)j_number(&j);
              else j_skip_value(&j);
              if (j.err) return 0;
              j_skip_ws(&j);
              if (j.p < j.end && *j.p == ',') { j.p++; continue; }
              if (j.p < j.end && *j.p == '}') { j.p++; break; }
              return 0;
            }
          } else if (str_eq(ikey, str_c("templates"))) {
            if (!j_expect(&j, '{')) return 0;
            j_skip_ws(&j);
            if (j.p < j.end && *j.p == '}') { j.p++; }
            else for (;;) {
              j_skip_ws(&j);
              if (j.p >= j.end || *j.p != '"') return 0;
              Str name = j_string(&j);
              if (j.err) return 0;
              if (!j_expect(&j, ':')) return 0;
              j_skip_ws(&j);
              Str src = j_string(&j);
              if (j.err) return 0;
              if (in.ntpls == tcap) {
                tcap = tcap ? tcap * 2 : 16;
                InTpl *nt = (InTpl *)ralloc(sizeof(InTpl) * tcap);
                if (in.tpls) memcpy(nt, in.tpls, sizeof(InTpl) * in.ntpls);
                in.tpls = nt;
              }
              in.tpls[in.ntpls].name = name; in.tpls[in.ntpls].src = src; in.ntpls++;
              j_skip_ws(&j);
              if (j.p < j.end && *j.p == ',') { j.p++; continue; }
              if (j.p < j.end && *j.p == '}') { j.p++; break; }
              return 0;
            }
          } else if (str_eq(ikey, str_c("files"))) {
            if (!j_expect(&j, '[')) return 0;
            j_skip_ws(&j);
            if (j.p < j.end && *j.p == ']') { j.p++; }
            else for (;;) {
              if (!j_expect(&j, '{')) return 0;
              Str path = str_make(0, 0), content = str_make(0, 0);
              j_skip_ws(&j);
              if (j.p < j.end && *j.p == '}') { j.p++; }
              else for (;;) {
                j_skip_ws(&j);
                if (j.p >= j.end || *j.p != '"') return 0;
                Str fkey = j_string(&j);
                if (j.err) return 0;
                if (!j_expect(&j, ':')) return 0;
                j_skip_ws(&j);
                if (str_eq(fkey, str_c("path"))) path = j_string(&j);
                else if (str_eq(fkey, str_c("content"))) content = j_string(&j);
                else j_skip_value(&j);
                if (j.err) return 0;
                j_skip_ws(&j);
                if (j.p < j.end && *j.p == ',') { j.p++; continue; }
                if (j.p < j.end && *j.p == '}') { j.p++; break; }
                return 0;
              }
              if (in.nfiles == fcap) {
                fcap = fcap ? fcap * 2 : 64;
                InFile *nf = (InFile *)ralloc(sizeof(InFile) * fcap);
                if (in.files) memcpy(nf, in.files, sizeof(InFile) * in.nfiles);
                in.files = nf;
              }
              in.files[in.nfiles].path = path; in.files[in.nfiles].content = content; in.nfiles++;
              j_skip_ws(&j);
              if (j.p < j.end && *j.p == ',') { j.p++; continue; }
              if (j.p < j.end && *j.p == ']') { j.p++; break; }
              return 0;
            }
          } else j_skip_value(&j);
          if (j.err) return 0;
          j_skip_ws(&j);
          if (j.p < j.end && *j.p == ',') { j.p++; continue; }
          if (j.p < j.end && *j.p == '}') { j.p++; break; }
          return 0;
        }
      } else j_skip_value(&j);
      if (j.err) return 0;
      j_skip_ws(&j);
      if (j.p < j.end && *j.p == ',') { j.p++; continue; }
      if (j.p < j.end && *j.p == '}') { j.p++; break; }
      return 0;
    }
    on_case(&in, ctx);
    j_skip_ws(&j);
    if (j.p < j.end && *j.p == ',') { j.p++; continue; }
    if (j.p < j.end && *j.p == ']') { j.p++; break; }
    return 0;
  }
  return j.err ? 0 : 1;
}

/* ---- emitter: JSON.stringify escaping into a raw Out cursor ---- */
static const char HEXD[] = "0123456789abcdef";
static __m128i JE_Q, JE_BS, JE_BIAS, JE_LIM;
static int JE_V_OK = 0;
static void json_emit_init(void) {
  if (JE_V_OK) return;
  JE_Q = _mm_set1_epi8('"'); JE_BS = _mm_set1_epi8('\\');
  JE_BIAS = _mm_set1_epi8((char)0x80); JE_LIM = _mm_set1_epi8((char)0xA0);
  JE_V_OK = 1;
}
/* Buf variant (error messages). */
static void json_escape_into(Buf *b, Str s) {
  const char *p = s.p, *end = s.p + s.n;
  const char *run = p;
  while (p < end) {
    const char *hit = end;
    while (end - p >= 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)p);
      __m128i m = _mm_cmpeq_epi8(v, JE_Q);
      m = _mm_or_si128(m, _mm_cmpeq_epi8(v, JE_BS));
      __m128i vb = _mm_xor_si128(v, JE_BIAS);
      m = _mm_or_si128(m, _mm_cmplt_epi8(vb, JE_LIM));
      int mask = _mm_movemask_epi8(m);
      if (mask) { hit = p + __builtin_ctz((unsigned)mask); break; }
      p += 16;
    }
    if (hit == end) {
      while (p < end) {
        u8 c = (u8)*p;
        if (c == '"' || c == '\\' || c < 0x20) { hit = p; break; }
        p++;
      }
      if (hit == end) break;
    }
    if (hit > run) buf_add(b, run, (u32)(hit - run));
    u8 c = (u8)*hit;
    switch (c) {
      case '"': buf_addlit(b, "\\\""); break;
      case '\\': buf_addlit(b, "\\\\"); break;
      case '\b': buf_addlit(b, "\\b"); break;
      case '\f': buf_addlit(b, "\\f"); break;
      case '\n': buf_addlit(b, "\\n"); break;
      case '\r': buf_addlit(b, "\\r"); break;
      case '\t': buf_addlit(b, "\\t"); break;
      default: {
        char tmp[6] = {'\\', 'u', '0', '0', 0, 0};
        tmp[4] = HEXD[(c >> 4) & 15]; tmp[5] = HEXD[c & 15];
        buf_add(b, tmp, 6);
        break;
      }
    }
    p = hit + 1;
    run = p;
  }
  if (end > run) buf_add(b, run, (u32)(end - run));
}
/* Raw-cursor JSON escape, single pass: clean 16-byte chunks are stored
 * straight to the output with one SIMD store (no per-run memcpy call);
 * only the bytes up to a hit are copied, and hits are escaped inline.
 * Same byte output as the run-based version.
 * Incremental (byte-local): each call is independent, so template
 * renders can stream escaped output straight into the answers cursor
 * (emit fusion) instead of buffering a whole page first. */
static void out_adds_esc(Out *o, Str s) {
  const char *p = s.p, *end = s.p + s.n;
  /* Reserve 2x: covers every 2-byte escape (", \, \n, \r, \t, \b, \f).
   * A 6-byte \u00XX escape is only possible for other control chars; those
   * are checked separately below (they do not occur in page HTML). */
  out_need(o, s.n * 2 + 64);
  char *dst = o->cur;
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_cmpeq_epi8(v, JE_Q);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, JE_BS));
    __m128i vb = _mm_xor_si128(v, JE_BIAS);
    m = _mm_or_si128(m, _mm_cmplt_epi8(vb, JE_LIM));
    int mask = _mm_movemask_epi8(m);
    if (!mask) { _mm_storeu_si128((__m128i *)dst, v); dst += 16; p += 16; continue; }
    int idx = __builtin_ctz((unsigned)mask);
    memcpy(dst, p, (usize)idx);
    dst += idx;
    u8 c = (u8)p[idx];
    switch (c) {
      case '"': dst[0] = '\\'; dst[1] = '"'; dst += 2; break;
      case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
      case '\b': dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
      case '\f': dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
      case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
      case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
      case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
      default:
        if ((usize)(o->end - dst) < 6) { o->cur = dst; out_need(o, 6); dst = o->cur; }
        dst[0] = '\\'; dst[1] = 'u'; dst[2] = '0'; dst[3] = '0';
        dst[4] = HEXD[(c >> 4) & 15]; dst[5] = HEXD[c & 15]; dst += 6;
        break;
    }
    p += idx + 1;
  }
  while (p < end) {
    u8 c = (u8)*p;
    if (c == '"' || c == '\\' || c < 0x20) {
      switch (c) {
        case '"': dst[0] = '\\'; dst[1] = '"'; dst += 2; break;
        case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
        case '\b': dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
        case '\f': dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
        case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
        case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
        case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
        default:
          if ((usize)(o->end - dst) < 6) { o->cur = dst; out_need(o, 6); dst = o->cur; }
          dst[0] = '\\'; dst[1] = 'u'; dst[2] = '0'; dst[3] = '0';
          dst[4] = HEXD[(c >> 4) & 15]; dst[5] = HEXD[c & 15]; dst += 6;
          break;
      }
      p++;
    } else {
      *dst++ = (char)c;
      p++;
    }
  }
  o->cur = dst;
}

/* Whole-string JSON escape into a raw Out cursor (same as out_adds_esc). */
static void json_escape_out(Out *o, Str s) { out_adds_esc(o, s); }

#endif
