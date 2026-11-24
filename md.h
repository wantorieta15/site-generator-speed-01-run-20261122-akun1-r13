/* Markdown: makefun-site's own subset (lib/markdown.js), streaming.
 * One pass over the body, one classification per line (first-byte dispatch),
 * blocks emitted straight into the page buffer. No line table, no
 * classification array, no per-block arena buffers: multi-line blocks are
 * assembled in one reusable scratch buffer (the join is a single '\n'
 * between trimmed lines, so inline constructs may still span lines).
 * Inline rendering is md2's single-scan algorithm with the SIMD needle
 * vectors hoisted out of the per-span calls.
 */
#ifndef SG_MD_H
#define SG_MD_H

#include "rt.h"
#include "text.h"
#include "fm.h"

typedef Str (*MdLinkFn)(void *ctx, Str target);

typedef struct { u8 level; Str id; Str html; } TocEnt;

typedef struct {
  TocEnt *toc; u32 ntoc, toc_cap;
  Str excerpt;
  int has_excerpt;
  u32 ex_s, ex_e; /* excerpt offsets into the page buffer (materialized at end) */
} MdResult;

/* Hoisted inline-scan needles: \ ` * [ & < > " ' */
static __m128i INL_V[9];
static int INL_V_OK = 0;
static void md_init(void) {
  if (INL_V_OK) return;
  INL_V[0] = _mm_set1_epi8('\\'); INL_V[1] = _mm_set1_epi8('`');
  INL_V[2] = _mm_set1_epi8('*'); INL_V[3] = _mm_set1_epi8('[');
  INL_V[4] = _mm_set1_epi8('&'); INL_V[5] = _mm_set1_epi8('<');
  INL_V[6] = _mm_set1_epi8('>'); INL_V[7] = _mm_set1_epi8('"');
  INL_V[8] = _mm_set1_epi8('\'');
  INL_V_OK = 1;
}

static const char *find_inline_or_esc(const char *p, const char *end) {
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_cmpeq_epi8(v, INL_V[0]);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[1]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[2]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[3]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[4]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[5]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[6]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[7]));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, INL_V[8]));
    int mask = _mm_movemask_epi8(m);
    if (mask) return p + __builtin_ctz((unsigned)mask);
    p += 16;
  }
  while (p < end) {
    char c = *p;
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return p;
    p++;
  }
  return end;
}

static const u8 ESCAPABLE_TAB[256] = {
  ['\\']=1, ['`']=1, ['*']=1, ['_']=1, ['[']=1, [']']=1, ['(']=1, [')']=1,
  ['#']=1, ['+']=1, ['-']=1, ['.']=1, ['!']=1, ['>']=1,
};
static int is_escapable(char c) { return ESCAPABLE_TAB[(u8)c]; }

/* Consecutive backslashes immediately before j, clamped to [from, j). */
static u32 md_bs_run(Str s, u32 from, u32 j) {
  u32 k = 0;
  while (j > from && s.p[j - 1] == '\\') { k++; j--; }
  return k;
}

static int md_find_unescaped(Str s, const char *what, u32 wlen, u32 from) {
  const char *p = s.p + from, *end = s.p + s.n;
  if (wlen == 2) {
    while (p < end) {
      usize off = memchr_idx(p, (usize)(end - p), '*');
      const char *hit = p + off;
      if (hit == end || hit + 1 >= end) return -1;
      if (hit[1] == '*') {
        u32 j = (u32)(hit - s.p);
        if ((md_bs_run(s, from, j) & 1) == 0) return (int)j;
      }
      p = hit + 1;
    }
    return -1;
  }
  while (p < end) {
    usize off = memchr_idx(p, (usize)(end - p), what[0]);
    const char *hit = p + off;
    if (hit == end) return -1;
    u32 j = (u32)(hit - s.p);
    if ((md_bs_run(s, from, j) & 1) == 0) return (int)j;
    p = hit + 1;
  }
  return -1;
}

static int md_find_single_star(Str s, u32 from) {
  const char *p = s.p + from, *end = s.p + s.n;
  while (p < end) {
    usize off = memchr_idx(p, (usize)(end - p), '*');
    const char *hit = p + off;
    if (hit == end) return -1;
    u32 j = (u32)(hit - s.p);
    if ((md_bs_run(s, from, j) & 1) == 1) { p = hit + 1; continue; }
    if (hit + 1 >= end || hit[1] != '*') return (int)j;
    p = hit + 2;
  }
  return -1;
}

static int md_has_ws(Str s) {
  if (str_is_ascii(s)) {
    for (u32 i = 0; i < s.n; i++) {
      char c = s.p[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return 1;
    }
    return 0;
  }
  u32 i = 0;
  while (i < s.n) {
    u8 c = (u8)s.p[i];
    if (c < 0x80) { if (is_js_ws_cp(c)) return 1; i++; }
    else { u32 cp; int n = utf8_decode(s.p + i, s.n - i, &cp); i += (u32)n; if (is_js_ws_cp(cp)) return 1; }
  }
  return 0;
}

static void md_inline(Out *out, Str s, MdLinkFn link, void *ctx) {
  const char *p = s.p, *end = s.p + s.n;
  while (p < end) {
    const char *hit = find_inline_or_esc(p, end);
    if (hit > p) { out_add(out, p, (usize)(hit - p)); p = hit; }
    if (p >= end) break;
    char c = *p;
    if (c == '&') { out_add(out, ESC_AMP, 5); p++; continue; }
    if (c == '<') { out_add(out, ESC_LT, 4); p++; continue; }
    if (c == '>') { out_add(out, ESC_GT, 4); p++; continue; }
    if (c == '"') { out_add(out, ESC_QUOT, 6); p++; continue; }
    if (c == '\'') { out_add(out, ESC_APOS, 5); p++; continue; }
    if (c == '\\' && p + 1 < end && is_escapable(p[1])) { escape_html_char_out(out, p[1]); p += 2; continue; }
    if (c == '`') {
      usize off = memchr_idx(p + 1, (usize)(end - (p + 1)), '`');
      const char *close = p + 1 + off;
      if (close != end && close > p + 1) {
        out_lit(out, "<code>");
        escape_html_out(out, str_make(p + 1, (u32)(close - (p + 1))));
        out_lit(out, "</code>");
        p = close + 1;
      } else { out_c(out, '`'); p++; }
      continue;
    }
    if (c == '*') {
      u32 at = (u32)(p - s.p);
      if (p + 1 < end && p[1] == '*') {
        int j = md_find_unescaped(s, "**", 2, at + 2);
        if (j > (int)at + 2) {
          out_lit(out, "<strong>");
          md_inline(out, str_make(p + 2, (u32)((s.p + j) - (p + 2))), link, ctx);
          out_lit(out, "</strong>");
          p = s.p + j + 2;
        } else { out_lit(out, "**"); p += 2; }
        continue;
      }
      int j = md_find_single_star(s, at + 1);
      if (j > (int)at + 1) {
        out_lit(out, "<em>");
        md_inline(out, str_make(p + 1, (u32)((s.p + j) - (p + 1))), link, ctx);
        out_lit(out, "</em>");
        p = s.p + j + 1;
      } else { out_c(out, '*'); p++; }
      continue;
    }
    if (c == '[') {
      u32 at = (u32)(p - s.p);
      int k = md_find_unescaped(s, "]", 1, at + 1);
      int m = -1;
      Str target = str_make(0, 0);
      if (k != -1 && (u32)k + 1 < s.n && s.p[k + 1] == '(') {
        usize off = memchr_idx(s.p + k + 2, (usize)(s.n - (k + 2)), ')');
        const char *rp = s.p + k + 2 + off;
        if (rp != s.p + s.n) { m = (int)(rp - s.p); target = str_make(s.p + k + 2, (u32)(m - (k + 2))); }
      }
      if (m != -1 && target.n > 0 && !md_has_ws(target)) {
        Str href = link(ctx, target);
        out_lit(out, "<a href=\"");
        escape_html_out(out, href);
        out_lit(out, "\">");
        md_inline(out, str_make(p + 1, (u32)((s.p + k) - (p + 1))), link, ctx);
        out_lit(out, "</a>");
        p = s.p + m + 1;
        continue;
      }
      out_c(out, '['); p++;
      continue;
    }
    out_c(out, c);
    p++;
  }
}

/* ---- line classification (one pass, first-byte dispatch) ---- */
enum { L_TEXT = 0, L_BLANK, L_FENCE, L_HEAD, L_HR, L_QUOTE, L_BULLET, L_ORD, L_CONT };

typedef struct {
  u8 kind;
  u8 depth;    /* HEAD: 1..6 */
  u8 fflags;   /* FENCE: bit0=is_open, bit1=is_close */
  u8 _pad;
  u32 aux;     /* ORD: start number */
  Str text;    /* HEAD/BULLET/ORD: item text; FENCE: lang */
} LineInfo;

static int md_fence_open(Str l, Str *lang) {
  if (l.n < 3 || memcmp(l.p, "```", 3) != 0) return 0;
  u32 j = 3;
  while (j < l.n) {
    char c = l.p[j];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') j++;
    else break;
  }
  for (u32 k = j; k < l.n; k++) if (!is_space_tab(l.p[k])) return 0;
  *lang = str_make(l.p + 3, j - 3);
  return 1;
}
static int md_fence_close(Str l) {
  if (l.n < 3 || memcmp(l.p, "```", 3) != 0) return 0;
  for (u32 i = 3; i < l.n; i++) if (!is_space_tab(l.p[i])) return 0;
  return 1;
}
static int md_heading(Str l, int *depth, Str *text) {
  u32 h = 0;
  while (h < l.n && l.p[h] == '#') h++;
  if (h == 0 || h > 6) return 0;
  if (h >= l.n || !is_space_tab(l.p[h])) return 0;
  u32 a = h;
  while (a < l.n && is_space_tab(l.p[a])) a++;
  Str t = str_make(l.p + a, l.n - a);
  while (t.n > 0 && is_space_tab(t.p[t.n - 1])) t.n--;
  if (t.n == 0) return 0;
  *depth = (int)h;
  *text = t;
  return 1;
}
static int md_hr(Str l) {
  if (l.n < 3) return 0;
  if (memcmp(l.p, "---", 3) != 0 && memcmp(l.p, "***", 3) != 0) return 0;
  for (u32 i = 3; i < l.n; i++) if (!is_space_tab(l.p[i])) return 0;
  return 1;
}
static int md_bullet(Str l, Str *text) {
  if (l.n < 2 || (l.p[0] != '-' && l.p[0] != '*') || l.p[1] != ' ') return 0;
  *text = str_make(l.p + 2, l.n - 2);
  return 1;
}
static int md_ordered(Str l, int *start, Str *text) {
  u32 i = 0;
  while (i < l.n && is_digit(l.p[i])) i++;
  if (i == 0 || i > 9) return 0;
  if (i + 1 >= l.n || l.p[i] != '.' || l.p[i + 1] != ' ') return 0;
  long v = 0;
  for (u32 k = 0; k < i; k++) v = v * 10 + (l.p[k] - '0');
  *start = (int)v;
  *text = str_make(l.p + i + 2, l.n - (i + 2));
  return 1;
}

/* Classify one line (span without the trailing '\n'). */
static void md_classify(Str l, LineInfo *inf) {
  inf->kind = L_TEXT; inf->depth = 0; inf->fflags = 0; inf->aux = 0;
  inf->text = str_make(0, 0);
  if (l.n == 0) { inf->kind = L_BLANK; return; }
  char c0 = l.p[0];
  if (c0 == ' ' || c0 == '\t') {
    /* Blank (all space/tab) or continued (>=2 spaces + non-ws). */
    u32 sp = 0;
    while (sp < l.n && l.p[sp] == ' ') sp++;
    u32 j = sp;
    int allws = 1;
    while (j < l.n) { if (!is_space_tab(l.p[j])) { allws = 0; break; } j++; }
    if (allws) { inf->kind = L_BLANK; return; }
    if (sp >= 2) {
      u8 c = (u8)l.p[sp];
      int nonws;
      if (c < 0x80) nonws = !is_js_ws_cp(c);
      else { u32 cp; utf8_decode(l.p + sp, l.n - sp, &cp); nonws = !is_js_ws_cp(cp); }
      if (nonws) { inf->kind = L_CONT; return; }
    }
    return;
  }
  if (c0 == '`') {
    Str lang; int is_open = md_fence_open(l, &lang); int is_close = md_fence_close(l);
    if (is_open || is_close) {
      inf->kind = L_FENCE;
      inf->fflags = (u8)((is_open ? 1 : 0) | (is_close ? 2 : 0));
      inf->text = lang;
    }
    return;
  }
  if (c0 == '#') {
    int depth; Str htext;
    if (md_heading(l, &depth, &htext)) { inf->kind = L_HEAD; inf->depth = (u8)depth; inf->text = htext; }
    return;
  }
  if (c0 == '-' || c0 == '*') {
    if (md_hr(l)) { inf->kind = L_HR; return; }
    Str bt;
    if (md_bullet(l, &bt)) { inf->kind = L_BULLET; inf->text = bt; }
    return;
  }
  if (c0 == '>') { inf->kind = L_QUOTE; return; }
  if (c0 >= '0' && c0 <= '9') {
    int os; Str bt;
    if (md_ordered(l, &os, &bt)) { inf->kind = L_ORD; inf->aux = (u32)os; inf->text = bt; }
    return;
  }
}

static int md_ends_para(u8 kind) {
  return kind == L_BLANK || kind == L_FENCE || kind == L_HEAD || kind == L_HR;
}

/* A small set of taken slugs (per page, for heading ids). Scratch array. */
typedef struct { Str *v; u32 n, cap; } StrSet;
static void strset_init(StrSet *s, u32 cap) { s->v = (Str *)salloc(sizeof(Str) * cap); s->n = 0; s->cap = cap; }
static int strset_has(StrSet *s, Str x) {
  for (u32 i = 0; i < s->n; i++) if (str_eq(s->v[i], x)) return 1;
  return 0;
}
static void strset_add(StrSet *s, Str x) {
  if (s->n == s->cap) {
    u32 nc = s->cap * 2;
    Str *nv = (Str *)salloc(sizeof(Str) * nc);
    memcpy(nv, s->v, sizeof(Str) * s->n);
    s->v = nv; s->cap = nc;
  }
  s->v[s->n++] = x;
}
/* uniqueSlug: the first of slug, slug-2, slug-3 ... not taken. The kept
 * copy lives in the retained arena (toc entries outlive the page). */
static Str md_unique_slug(StrSet *taken, Str slug, char *tmp) {
  u32 base = slug.n;
  memcpy(tmp, slug.p, base);
  tmp[base] = 0;
  Str cur = str_make(tmp, base);
  if (!strset_has(taken, cur)) { Str keep = str_make((const char *)ralloc(base + 1), base); memcpy((void *)keep.p, tmp, base); strset_add(taken, keep); return keep; }
  for (int n = 2;; n++) {
    u32 at = base;
    tmp[at++] = '-';
    char rev[12]; int rn = 0, v = n;
    do { rev[rn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (rn) tmp[at++] = rev[--rn];
    tmp[at] = 0;
    Str cand = str_make(tmp, at);
    if (!strset_has(taken, cand)) { Str keep = str_make((const char *)ralloc(at + 1), at); memcpy((void *)keep.p, tmp, at); strset_add(taken, keep); return keep; }
  }
}

/* ---- the streaming renderer ---- */
/* Scratch toc entries (offsets into the page buffer; materialized at end). */
typedef struct { u8 level; Str id; u32 off, len; } TocTmp;

static MdResult md_render(Str body, MdLinkFn link, void *ctx, Out *out, Buf *scratch) {
  MdResult r; r.toc = 0; r.ntoc = 0; r.toc_cap = 0; r.excerpt = str_make(0, 0); r.has_excerpt = 0; r.ex_s = 0; r.ex_e = 0;
  TocTmp *tt = (TocTmp *)salloc(sizeof(TocTmp) * 8);
  u32 ntt = 0, tcap = 8;
  StrSet ids; strset_init(&ids, 16);
  char slugbuf[80];

  const char *bp = body.p;
  u32 bn = body.n;
  u32 pos = 0;               /* offset of the current line's start */
  u32 line_end = bn;         /* offset of the '\n' ending the current line (or bn) */
  int has = bn > 0;
  Str line = str_make(0, 0);
  LineInfo inf;
  /* Load the first line. */
  if (has) {
    usize nl = memchr_idx(bp, bn, '\n');
    line_end = nl >= bn ? bn : (u32)nl;
    line = str_make(bp, line_end);
    md_classify(line, &inf);
  }
  /* Advance to the next line (pos moves past the current line's '\n').
   * Only ONE memchr per line: the current line's end is already known
   * in line_end, so we only scan for the next line's end. */
#define MD_ADV() do { \
    if (line_end >= bn) { pos = bn; has = 0; } \
    else { pos = line_end + 1; \
      usize nl2 = memchr_idx(bp + pos, bn - pos, '\n'); \
      line_end = nl2 >= bn - pos ? bn : pos + (u32)nl2; \
      line = str_make(bp + pos, line_end - pos); \
      md_classify(line, &inf); } \
  } while (0)

  while (has) {
    if (inf.kind == L_BLANK) { MD_ADV(); continue; }
    if (inf.kind == L_FENCE && (inf.fflags & 1)) {
      /* Fence: content is the contiguous range after this line up to the
       * close line's start (or the body end). */
      Str lang = inf.text;
      u32 cs = line_end < bn ? line_end + 1 : bn;
      MD_ADV();
      u32 ce = bn;
      while (has) {
        if (inf.kind == L_FENCE && (inf.fflags & 2)) { ce = pos; break; }
        MD_ADV();
      }
      out_lit(out, "<pre><code");
      if (lang.n > 0) { out_lit(out, " class=\"language-"); escape_html_out(out, lang); out_c(out, '"'); }
      out_c(out, '>');
      if (ce > cs) {
        escape_html_out(out, str_make(bp + cs, ce - cs));
        if (bp[ce - 1] != '\n') out_c(out, '\n');
      }
      out_lit(out, "</code></pre>\n");
      if (has) MD_ADV(); /* past the close line */
      continue;
    }
    if (inf.kind == L_HEAD) {
      char sb[80];
      u32 sl = slugify(sb, inf.text);
      Str id = md_unique_slug(&ids, str_make(sb, sl), slugbuf);
      out_lit(out, "<h");
      out_c(out, (char)('0' + inf.depth));
      out_lit(out, " id=\"");
      out_adds(out, id);
      out_lit(out, "\">");
      u32 hs = (u32)(out->cur - out->p);
      md_inline(out, inf.text, link, ctx);
      u32 he = (u32)(out->cur - out->p);
      out_lit(out, "</h");
      out_c(out, (char)('0' + inf.depth));
      out_lit(out, ">\n");
      if (inf.depth == 2 || inf.depth == 3) {
        if (ntt == tcap) {
          tcap *= 2;
          TocTmp *nt = (TocTmp *)salloc(sizeof(TocTmp) * tcap);
          memcpy(nt, tt, sizeof(TocTmp) * ntt);
          tt = nt;
        }
        tt[ntt].level = inf.depth; tt[ntt].id = id; tt[ntt].off = hs; tt[ntt].len = he - hs; ntt++;
      }
      MD_ADV();
      continue;
    }
    if (inf.kind == L_HR) { out_lit(out, "<hr>\n"); MD_ADV(); continue; }
    if (inf.kind == L_QUOTE) {
      scratch->n = 0;
      int any = 0;
      while (has && inf.kind == L_QUOTE) {
        Str l = line;
        u32 a = 1;
        if (a < l.n && l.p[a] == ' ') a++;
        Str t = trim_space_tab(str_make(l.p + a, l.n - a));
        if (t.n > 0) {
          if (any) buf_addc(scratch, '\n');
          buf_adds(scratch, t);
          any = 1;
        }
        MD_ADV();
      }
      if (!any) out_lit(out, "<blockquote>\n</blockquote>\n");
      else {
        out_lit(out, "<blockquote>\n<p>");
        md_inline(out, buf_str(scratch), link, ctx);
        out_lit(out, "</p>\n</blockquote>\n");
      }
      continue;
    }
    if (inf.kind == L_BULLET || inf.kind == L_ORD) {
      int ordered = inf.kind == L_ORD;
      int ostart = (int)inf.aux;
      out_lit(out, ordered ? "<ol" : "<ul");
      if (ordered && ostart != 1) {
        out_lit(out, " start=\"");
        char tmp[12]; int tn = 0, v = ostart;
        char rev[12]; int rn = 0; do { rev[rn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (rn) tmp[tn++] = rev[--rn];
        out_add(out, tmp, (u32)tn);
        out_c(out, '"');
      }
      out_lit(out, ">\n");
      for (;;) {
        Str item = trim_space_tab(inf.text);
        scratch->n = 0;
        buf_adds(scratch, item);
        MD_ADV();
        for (;;) {
          if (!has) break;
          if (ordered ? (inf.kind == L_ORD) : (inf.kind == L_BULLET)) break;
          if (inf.kind == L_CONT) {
            buf_addc(scratch, '\n');
            buf_adds(scratch, trim_space_tab(line));
            MD_ADV();
            continue;
          }
          break;
        }
        out_lit(out, "<li>");
        md_inline(out, buf_str(scratch), link, ctx);
        out_lit(out, "</li>\n");
        if (!has) break;
        if (ordered ? (inf.kind == L_ORD) : (inf.kind == L_BULLET)) continue;
        break;
      }
      out_lit(out, ordered ? "</ol>\n" : "</ul>\n");
      continue;
    }
    /* Paragraph: lines until a BLANK / FENCE / HEAD / HR.
     * Fast path: when every line is already trimmed (no leading/trailing
     * space/tab), the paragraph is the contiguous body slice from the
     * first line's start to the last line's end -- no scratch copy.
     * Otherwise re-walk from the paragraph start into scratch (the
     * iterator state after the re-walk matches the lookahead exactly). */
    {
      u32 pstart = pos;
      u32 pend = line_end;
      int clean = line.n > 0 && !is_space_tab(line.p[0]) && !is_space_tab(line.p[line.n - 1]);
      MD_ADV();
      while (has && !md_ends_para(inf.kind)) {
        if (line.n == 0 || is_space_tab(line.p[0]) || is_space_tab(line.p[line.n - 1])) clean = 0;
        pend = line_end;
        MD_ADV();
      }
      out_lit(out, "<p>");
      u32 ps = (u32)(out->cur - out->p);
      if (clean) {
        md_inline(out, str_make(bp + pstart, pend - pstart), link, ctx);
      } else {
        scratch->n = 0;
        pos = pstart; has = 1;
        {
          usize nl = memchr_idx(bp + pos, bn - pos, '\n');
          line_end = nl >= bn - pos ? bn : pos + (u32)nl;
          line = str_make(bp + pos, line_end - pos);
          md_classify(line, &inf);
        }
        buf_adds(scratch, trim_space_tab(line));
        MD_ADV();
        while (has && !md_ends_para(inf.kind)) {
          buf_addc(scratch, '\n');
          buf_adds(scratch, trim_space_tab(line));
          MD_ADV();
        }
        md_inline(out, buf_str(scratch), link, ctx);
      }
      u32 pe = (u32)(out->cur - out->p);
      out_lit(out, "</p>\n");
      if (!r.has_excerpt) { r.ex_s = ps; r.ex_e = pe; r.has_excerpt = 1; }
      continue;
    }
  }
#undef MD_ADV

  /* Materialize toc + excerpt against the final buffer base. */
  if (ntt) {
    r.toc = (TocEnt *)ralloc(sizeof(TocEnt) * ntt);
    r.ntoc = ntt; r.toc_cap = ntt;
    for (u32 i = 0; i < ntt; i++) {
      r.toc[i].level = tt[i].level;
      r.toc[i].id = tt[i].id;
      r.toc[i].html = str_make(out->p + tt[i].off, tt[i].len);
    }
  }
  if (r.has_excerpt) r.excerpt = str_make(out->p + r.ex_s, r.ex_e - r.ex_s);
  return r;
}

#endif
