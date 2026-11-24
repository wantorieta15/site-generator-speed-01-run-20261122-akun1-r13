/* Text: HTML escaping, slugify (NFKD), long dates, word counting.
 * Mirrors makefun-site's lib/text.js exactly. The SIMD needle vectors are
 * hoisted to file scope and built once (the V6 versions rebuilt 5-9 set1
 * vectors on EVERY call, which dominated short-span scans).
 */
#ifndef SG_TEXT_H
#define SG_TEXT_H

#include "rt.h"
#include "nfkd.h"

static const char *ESC_AMP = "&amp;", *ESC_LT = "&lt;", *ESC_GT = "&gt;", *ESC_QUOT = "&quot;", *ESC_APOS = "&#39;";

/* Hoisted SIMD needles (built once by text_init). */
static __m128i ESC_V[5];   /* & < > " ' */
static int ESC_V_OK = 0;
static void text_init(void) {
  if (ESC_V_OK) return;
  ESC_V[0] = _mm_set1_epi8('&'); ESC_V[1] = _mm_set1_epi8('<');
  ESC_V[2] = _mm_set1_epi8('>'); ESC_V[3] = _mm_set1_epi8('"');
  ESC_V[4] = _mm_set1_epi8('\'');
  ESC_V_OK = 1;
}

/* First index in [p, end) holding one of & < > " ', or end. */
static const char *find_esc(const char *p, const char *end) {
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_cmpeq_epi8(v, ESC_V[0]);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[1]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[2]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[3]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[4]));
    int mask = _mm_movemask_epi8(m);
    if (mask) return p + __builtin_ctz((unsigned)mask);
    p += 16;
  }
  while (p < end) {
    char c = *p;
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return p;
    p++;
  }
  return end;
}

static void escape_html_into(Buf *b, Str s) {
  const char *p = s.p, *end = s.p + s.n, *run = p;
  for (;;) {
    const char *hit = find_esc(p, end);
    if (hit == end) { if (end > run) buf_add(b, run, (u32)(end - run)); return; }
    if (hit > run) buf_add(b, run, (u32)(hit - run));
    switch (*hit) {
      case '&': buf_add(b, ESC_AMP, 5); break;
      case '<': buf_add(b, ESC_LT, 4); break;
      case '>': buf_add(b, ESC_GT, 4); break;
      case '"': buf_add(b, ESC_QUOT, 6); break;
      default: buf_add(b, ESC_APOS, 5); break;
    }
    p = hit + 1; run = p;
  }
}

/* Same escape, straight into a raw Out cursor. */
static void escape_html_out(Out *o, Str s) {
  const char *p = s.p, *end = s.p + s.n, *run = p;
  for (;;) {
    const char *hit = find_esc(p, end);
    if (hit == end) { if (end > run) out_add(o, run, (usize)(end - run)); return; }
    if (hit > run) out_add(o, run, (usize)(hit - run));
    switch (*hit) {
      case '&': out_add(o, ESC_AMP, 5); break;
      case '<': out_add(o, ESC_LT, 4); break;
      case '>': out_add(o, ESC_GT, 4); break;
      case '"': out_add(o, ESC_QUOT, 6); break;
      default: out_add(o, ESC_APOS, 5); break;
    }
    p = hit + 1; run = p;
  }
}

static void escape_html_char(Buf *b, char c) {
  switch (c) {
    case '&': buf_add(b, ESC_AMP, 5); break;
    case '<': buf_add(b, ESC_LT, 4); break;
    case '>': buf_add(b, ESC_GT, 4); break;
    case '"': buf_add(b, ESC_QUOT, 6); break;
    case '\'': buf_add(b, ESC_APOS, 5); break;
    default: buf_addc(b, c); break;
  }
}

/* One character, escaped, into a raw Out cursor. */
static void escape_html_char_out(Out *o, char c) {
  switch (c) {
    case '&': out_add(o, ESC_AMP, 5); break;
    case '<': out_add(o, ESC_LT, 4); break;
    case '>': out_add(o, ESC_GT, 4); break;
    case '"': out_add(o, ESC_QUOT, 6); break;
    case '\'': out_add(o, ESC_APOS, 5); break;
    default: out_c(o, c); break;
  }
}

/* ---- slugify ---- */
#define SLUG_MAX 64

static int is_mark_cp(u32 cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
         (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
         (cp >= 0xFE20 && cp <= 0xFE2F);
}

static const NfkdEnt *nfkd_lookup(u32 cp) {
  int lo = 0, hi = NFKD_TAB_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (NFKD_TAB[mid].cp == cp) return &NFKD_TAB[mid];
    if (NFKD_TAB[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
  }
  return 0;
}

typedef struct { char *out; u32 len; int gap; } SlugState;

static void slug_feed(SlugState *st, u32 cp) {
  if (st->len >= 79) return;
  if (cp < 128) {
    char c = (char)cp;
    char keep = 0;
    if (c >= 'A' && c <= 'Z') keep = (char)(c + 32);
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = c;
    if (!keep) { st->gap = st->len > 0; return; }
    if (st->gap) { st->out[st->len++] = '-'; st->gap = 0; }
    st->out[st->len++] = keep;
    return;
  }
  if (is_mark_cp(cp)) return;
  st->gap = st->len > 0;
}

static u32 slugify(char *out, Str s) {
  SlugState st; st.out = out; st.len = 0; st.gap = 0;
  int ascii = str_is_ascii(s);
  if (ascii) {
    for (u32 i = 0; i < s.n; i++) slug_feed(&st, (u8)s.p[i]);
  } else {
    u32 i = 0;
    while (i < s.n) {
      u32 cp; int n = utf8_decode(s.p + i, s.n - i, &cp);
      i += (u32)n;
      if (cp < 128) { slug_feed(&st, cp); continue; }
      const NfkdEnt *e = nfkd_lookup(cp);
      if (!e) {
        if (is_mark_cp(cp)) continue;
        st.gap = st.len > 0;
        continue;
      }
      u32 j = 0;
      while (j < e->n) {
        u32 c2; int n2 = utf8_decode(e->rep + j, e->n - j, &c2);
        j += (u32)n2;
        slug_feed(&st, c2);
      }
    }
  }
  u32 len = st.len > SLUG_MAX ? SLUG_MAX : st.len;
  while (len > 0 && out[len - 1] == '-') len--;
  if (len == 0) { memcpy(out, "untitled", 8); return 8; }
  return len;
}

/* ---- long dates ---- */
static const char *MONTHS[] = {"January", "February", "March", "April", "May", "June",
                               "July", "August", "September", "October", "November", "December"};
static void long_date_into(Buf *b, Str date) {
  int d = (date.p[8] - '0') * 10 + (date.p[9] - '0');
  int m = (date.p[5] - '0') * 10 + (date.p[6] - '0');
  char tmp[8]; int n = 0;
  if (d >= 10) tmp[n++] = (char)('0' + d / 10);
  tmp[n++] = (char)('0' + d % 10);
  buf_add(b, tmp, (u32)n);
  buf_addc(b, ' ');
  const char *mn = MONTHS[m - 1];
  buf_add(b, mn, (u32)strlen(mn));
  buf_addc(b, ' ');
  buf_add(b, date.p, 4);
}

/* ---- JS whitespace ---- */
static int is_js_ws_cp(u32 cp) {
  return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20 ||
         cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

/* Word count (JS wordsOf). SIMD fast path for all-ASCII 16-byte chunks.
 * V18: whitespace {0x09..0x0D, 0x20} as one unsigned range check
 * ((c-9) <= 4, via the xor-0x80 signed-compare trick) plus a space
 * compare -- 5 vector ops per chunk instead of 6 cmpeq + 5 or. */
static long words_of(Str s) {
  long count = 0;
  int prev_ws = 1;
  u32 i = 0;
  const char *p = s.p;
  __m128i sp = _mm_set1_epi8(' ');
  __m128i nine = _mm_set1_epi8(9);
  __m128i bias = _mm_set1_epi8((char)0x80);
  __m128i lim = _mm_set1_epi8((char)0x85);
  __m128i zero = _mm_setzero_si128();
  while (i < s.n) {
    if (i + 16 <= s.n) {
      __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
      int himask = _mm_movemask_epi8(_mm_cmplt_epi8(v, zero));
      if (!himask) {
        __m128i t = _mm_sub_epi8(v, nine);
        __m128i m = _mm_cmplt_epi8(_mm_xor_si128(t, bias), lim); /* 0x09..0x0D */
        m = _mm_or_si128(m, _mm_cmpeq_epi8(v, sp));
        int wmask = _mm_movemask_epi8(m);
        int prev = ((wmask << 1) & 0xFFFF) | (prev_ws ? 1 : 0);
        int starts = (~wmask) & prev & 0xFFFF;
        count += __builtin_popcount((unsigned)starts);
        prev_ws = (wmask >> 15) & 1;
        i += 16;
        continue;
      }
    }
    u8 c = (u8)p[i];
    if (c < 0x80) {
      int ws = c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
      if (ws) prev_ws = 1;
      else if (prev_ws) { count++; prev_ws = 0; }
      i++;
    } else {
      u32 cp; int n = utf8_decode(p + i, s.n - i, &cp);
      i += (u32)n;
      if (is_js_ws_cp(cp)) prev_ws = 1;
      else if (prev_ws) { count++; prev_ws = 0; }
    }
  }
  return count;
}

static Str js_trim(Str s) {
  u32 a = 0, b = s.n;
  while (a < b) {
    u8 c = (u8)s.p[a];
    if (c < 0x80) { if (!is_js_ws_cp(c)) break; a++; }
    else { u32 cp; int n = utf8_decode(s.p + a, b - a, &cp); if (!is_js_ws_cp(cp)) break; a += (u32)n; }
  }
  while (b > a) {
    u32 j = b - 1;
    while (j > a && ((u8)s.p[j] & 0xC0) == 0x80) j--;
    u8 c = (u8)s.p[j];
    if (c < 0x80) { if (!is_js_ws_cp(c)) break; b = j; }
    else { u32 cp; int n = utf8_decode(s.p + j, b - j, &cp); (void)n; if (!is_js_ws_cp(cp)) break; b = j; }
  }
  return str_make(s.p + a, b - a);
}

static Str trim_space_tab(Str s) {
  u32 a = 0, b = s.n;
  while (a < b && is_space_tab(s.p[a])) a++;
  while (b > a && is_space_tab(s.p[b - 1])) b--;
  return str_make(s.p + a, b - a);
}

#endif
