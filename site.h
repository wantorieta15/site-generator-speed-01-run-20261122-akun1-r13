/* The build pipeline: makefun-site's lib/index.js, plugins in order.
 * readSources -> frontMatter -> permalinks -> markdown -> collections ->
 * tags -> layouts -> report.
 *
 * Fused differences vs the V6 port (same output, byte for byte):
 *  - The answers object is streamed straight into the output cursor
 *    (no per-entry OutEnt list, no second pass over the whole answer).
 *  - Templates are compiled lazily per render context, with includes
 *    inlined at compile time; constant templates are pre-rendered once
 *    per case and spliced in. All templates are still compiled once up
 *    front (CTX_PAGE) so a broken template is reported exactly like the
 *    oracle's upfront compile.
 *  - Sorts are quicksort (the keys are total orders, so stability is
 *    irrelevant), not the oracle's insertion sorts.
 *  - The page HTML and the markdown content use two persistent per-case
 *    output cursors (the layout reads page.content while it writes, so
 *    they cannot share one buffer).
 */
#ifndef SG_SITE_H
#define SG_SITE_H

#include "rt.h"
#include "json.h"
#include "text.h"
#include "fm.h"
#include "md.h"
#include "tpl.h"
#include "data.h"
#include "render.h"

/* ---- sorts (quicksort; keys are total orders) ---- */
/* V26: memcmp-first comparison. Byte order equals str_cmp_cu's
 * UTF-16 code-unit order whenever the first differing byte is ASCII
 * on both sides; a non-ASCII difference, or a common prefix containing
 * a high byte (which could end mid-character), falls back to the exact
 * str_cmp_cu. str_cmp_cu re-scans both strings for ASCII-ness on
 * every call; this skips those scans. */
static inline __attribute__((always_inline)) int cmp_src_fast(Str a, Str b) {
  u32 n = a.n < b.n ? a.n : b.n;
  u32 i = 0;
  int has_hi = 0;
  while (i + 8 <= n) {
    u64 x, y;
    memcpy(&x, a.p + i, 8); memcpy(&y, b.p + i, 8);
    if ((x | y) & 0x8080808080808080ull) has_hi = 1;
    if (x != y) {
      for (int k = 0; k < 8; k++) {
        u8 ca = (u8)a.p[i + (u32)k], cb = (u8)b.p[i + (u32)k];
        if (ca != cb) {
          if (ca < 0x80 && cb < 0x80) return ca < cb ? -1 : 1;
          return str_cmp_cu(a, b);
        }
      }
    }
    i += 8;
  }
  while (i < n) {
    u8 ca = (u8)a.p[i], cb = (u8)b.p[i];
    if (ca != cb) {
      if (ca < 0x80 && cb < 0x80) return ca < cb ? -1 : 1;
      return str_cmp_cu(a, b);
    }
    if (ca >= 0x80) has_hi = 1;
    i++;
  }
  if (has_hi) return str_cmp_cu(a, b);
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

static void sort_infiles(InFile *a, u32 n) {
  if (n < 2) return;
  /* Iterative quicksort with median-of-three pivot, insertion sort for
   * small runs. */
  u32 stack[64][2]; int sp = 0;
  stack[sp][0] = 0; stack[sp][1] = n - 1; sp++;
  while (sp > 0) {
    sp--;
    u32 lo = stack[sp][0], hi = stack[sp][1];
    while (hi - lo >= 16) {
      u32 mid = lo + (hi - lo) / 2;
      if (cmp_src_fast(a[mid].path, a[lo].path) < 0) { InFile t = a[lo]; a[lo] = a[mid]; a[mid] = t; }
      if (cmp_src_fast(a[hi].path, a[lo].path) < 0) { InFile t = a[lo]; a[lo] = a[hi]; a[hi] = t; }
      if (cmp_src_fast(a[mid].path, a[hi].path) < 0) { InFile t = a[mid]; a[mid] = a[hi]; a[hi] = t; }
      InFile piv = a[mid];
      u32 i = lo, j = hi;
      for (;;) {
        while (cmp_src_fast(a[i].path, piv.path) < 0) i++;
        while (cmp_src_fast(a[j].path, piv.path) > 0) j--;
        if (i >= j) break;
        InFile t = a[i]; a[i] = a[j]; a[j] = t; i++; if (j == 0) break; j--;
      }
      if (j - lo < hi - i) {
        if (i < hi) { stack[sp][0] = i; stack[sp][1] = hi; sp++; }
        hi = j;
      } else {
        if (lo < j) { stack[sp][0] = lo; stack[sp][1] = j; sp++; }
        lo = i;
      }
    }
    for (u32 i = lo + 1; i <= hi; i++) { InFile x = a[i]; u32 j = i; while (j > lo && cmp_src_fast(a[j - 1].path, x.path) > 0) { a[j] = a[j - 1]; j--; } a[j] = x; }
  }
}

static void sort_intpls(InTpl *a, u32 n) {
  if (n < 2) return;
  u32 stack[64][2]; int sp = 0;
  stack[sp][0] = 0; stack[sp][1] = n - 1; sp++;
  while (sp > 0) {
    sp--;
    u32 lo = stack[sp][0], hi = stack[sp][1];
    while (hi - lo >= 16) {
      u32 mid = lo + (hi - lo) / 2;
      if (cmp_src_fast(a[mid].name, a[lo].name) < 0) { InTpl t = a[lo]; a[lo] = a[mid]; a[mid] = t; }
      if (cmp_src_fast(a[hi].name, a[lo].name) < 0) { InTpl t = a[lo]; a[lo] = a[hi]; a[hi] = t; }
      if (cmp_src_fast(a[mid].name, a[hi].name) < 0) { InTpl t = a[mid]; a[mid] = a[hi]; a[hi] = t; }
      InTpl piv = a[mid];
      u32 i = lo, j = hi;
      for (;;) {
        while (cmp_src_fast(a[i].name, piv.name) < 0) i++;
        while (cmp_src_fast(a[j].name, piv.name) > 0) j--;
        if (i >= j) break;
        InTpl t = a[i]; a[i] = a[j]; a[j] = t; i++; if (j == 0) break; j--;
      }
      if (j - lo < hi - i) {
        if (i < hi) { stack[sp][0] = i; stack[sp][1] = hi; sp++; }
        hi = j;
      } else {
        if (lo < j) { stack[sp][0] = lo; stack[sp][1] = j; sp++; }
        lo = i;
      }
    }
    for (u32 i = lo + 1; i <= hi; i++) { InTpl x = a[i]; u32 j = i; while (j > lo && cmp_src_fast(a[j - 1].name, x.name) > 0) { a[j] = a[j - 1]; j--; } a[j] = x; }
  }
}

static int post_cmp(Post *x, Post *y) {
  int c = cmp_src_fast(y->date, x->date); /* date desc */
  if (c) return c;
  return cmp_src_fast(x->slug, y->slug); /* then slug asc */
}

static void sort_posts(Post **a, u32 n) {
  if (n < 2) return;
  u32 stack[64][2]; int sp = 0;
  stack[sp][0] = 0; stack[sp][1] = n - 1; sp++;
  while (sp > 0) {
    sp--;
    u32 lo = stack[sp][0], hi = stack[sp][1];
    while (hi - lo >= 16) {
      u32 mid = lo + (hi - lo) / 2;
      if (post_cmp(a[mid], a[lo]) < 0) { Post *t = a[lo]; a[lo] = a[mid]; a[mid] = t; }
      if (post_cmp(a[hi], a[lo]) < 0) { Post *t = a[lo]; a[lo] = a[hi]; a[hi] = t; }
      if (post_cmp(a[mid], a[hi]) < 0) { Post *t = a[mid]; a[mid] = a[hi]; a[hi] = t; }
      Post *piv = a[mid];
      u32 i = lo, j = hi;
      for (;;) {
        while (post_cmp(a[i], piv) < 0) i++;
        while (post_cmp(a[j], piv) > 0) j--;
        if (i >= j) break;
        Post *t = a[i]; a[i] = a[j]; a[j] = t; i++; if (j == 0) break; j--;
      }
      if (j - lo < hi - i) {
        if (i < hi) { stack[sp][0] = i; stack[sp][1] = hi; sp++; }
        hi = j;
      } else {
        if (lo < j) { stack[sp][0] = lo; stack[sp][1] = j; sp++; }
        lo = i;
      }
    }
    for (u32 i = lo + 1; i <= hi; i++) { Post *x = a[i]; u32 j = i; while (j > lo && post_cmp(a[j - 1], x) > 0) { a[j] = a[j - 1]; j--; } a[j] = x; }
  }
}

static int tag_cmp(Tag *x, Tag *y) {
  if (x->nposts != y->nposts) return x->nposts > y->nposts ? -1 : 1; /* count desc */
  return cmp_src_fast(x->slug, y->slug); /* then slug asc */
}

static void sort_tagptrs(Tag **a, u32 n) {
  if (n < 2) return;
  u32 stack[64][2]; int sp = 0;
  stack[sp][0] = 0; stack[sp][1] = n - 1; sp++;
  while (sp > 0) {
    sp--;
    u32 lo = stack[sp][0], hi = stack[sp][1];
    while (hi - lo >= 16) {
      u32 mid = lo + (hi - lo) / 2;
      if (tag_cmp(a[mid], a[lo]) < 0) { Tag *t = a[lo]; a[lo] = a[mid]; a[mid] = t; }
      if (tag_cmp(a[hi], a[lo]) < 0) { Tag *t = a[lo]; a[lo] = a[hi]; a[hi] = t; }
      if (tag_cmp(a[mid], a[hi]) < 0) { Tag *t = a[mid]; a[mid] = a[hi]; a[hi] = t; }
      Tag *piv = a[mid];
      u32 i = lo, j = hi;
      for (;;) {
        while (tag_cmp(a[i], piv) < 0) i++;
        while (tag_cmp(a[j], piv) > 0) j--;
        if (i >= j) break;
        Tag *t = a[i]; a[i] = a[j]; a[j] = t; i++; if (j == 0) break; j--;
      }
      if (j - lo < hi - i) {
        if (i < hi) { stack[sp][0] = i; stack[sp][1] = hi; sp++; }
        hi = j;
      } else {
        if (lo < j) { stack[sp][0] = lo; stack[sp][1] = j; sp++; }
        lo = i;
      }
    }
    for (u32 i = lo + 1; i <= hi; i++) { Tag *x = a[i]; u32 j = i; while (j > lo && tag_cmp(a[j - 1], x) > 0) { a[j] = a[j - 1]; j--; } a[j] = x; }
  }
}

static void sort_strs(Str *a, u32 n) {
  for (u32 i = 1; i < n; i++) { Str x = a[i]; u32 j = i; while (j > 0 && cmp_src_fast(a[j - 1], x) > 0) { a[j] = a[j - 1]; j--; } a[j] = x; }
}

/* ---- the build state ---- */
typedef struct {
  Input *in;
  Str base;
  Tpl *tpls; u32 ntpls;
  RCtx rc;
  Str *report; u32 nreport, report_cap;
  Post **sources; u32 nsources;
  InFile *statics; u32 nstatics;
  Post **posts; u32 nposts;
  Tag **tags; u32 ntags, tags_cap;
  TagSum *all_tags;
  StrSet broken;
  Str *emitted; u32 nemitted, emitted_cap;
  int first_entry;
  Post *cur_src;
  Str cur_dir;
  /* V4: cur_dir split once per page (resolve_path reuses it). ok = 0
   * when the dir has >16 segments (falls back to resolve_path). */
  Str dir_parts[16];
  u32 ndir_parts;
  int dir_parts_ok;
  Out *ans;
  Out content_out;
  Buf md_scratch;
} Build;

static void report_push(Build *b, Str line) {
  if (b->nreport == b->report_cap) {
    u32 nc = b->report_cap ? b->report_cap * 2 : 32;
    Str *nv = (Str *)ralloc(sizeof(Str) * nc);
    if (b->report) memcpy(nv, b->report, sizeof(Str) * b->nreport);
    b->report = nv; b->report_cap = nc;
  }
  b->report[b->nreport++] = line;
}

/* Stream one answers entry ("path":"content") into the output cursor,
 * and record the path for the statics shadowing check. */
static void emit_entry(Build *b, Str path, Str content) {
  Out *o = b->ans;
  if (!b->first_entry) out_c(o, ',');
  b->first_entry = 0;
  out_c(o, '"');
  json_escape_out(o, path);
  out_lit(o, "\":\"");
  json_escape_out(o, content);
  out_c(o, '"');
  if (b->nemitted == b->emitted_cap) {
    u32 nc = b->emitted_cap ? b->emitted_cap * 2 : 64;
    Str *nv = (Str *)ralloc(sizeof(Str) * nc);
    if (b->emitted) memcpy(nv, b->emitted, sizeof(Str) * b->nemitted);
    b->emitted = nv; b->emitted_cap = nc;
  }
  b->emitted[b->nemitted++] = path;
}

/* Fused emit: open an answers entry and point the template renderer
 * straight at the answers cursor (JSON-escaped). The page is never
 * buffered; emit_end closes the string. Records the path like emit_entry. */
static void emit_begin(Build *b, Str path) {
  Out *o = b->ans;
  if (!b->first_entry) out_c(o, ',');
  b->first_entry = 0;
  out_c(o, '"');
  json_escape_out(o, path);
  out_lit(o, "\":\"");
  if (b->nemitted == b->emitted_cap) {
    u32 nc = b->emitted_cap ? b->emitted_cap * 2 : 64;
    Str *nv = (Str *)ralloc(sizeof(Str) * nc);
    if (b->emitted) memcpy(nv, b->emitted, sizeof(Str) * b->nemitted);
    b->emitted = nv; b->emitted_cap = nc;
  }
  b->emitted[b->nemitted++] = path;
  b->rc.out = b->ans;
  b->rc.out_esc = 1;
}
static void emit_end(Build *b) {
  out_c(b->ans, '"');
  b->rc.out_esc = 0;
}

static int out_has(Build *b, Str path) {
  for (u32 i = 0; i < b->nemitted; i++) if (str_eq(b->emitted[i], path)) return 1;
  return 0;
}

static int tpl_index(Build *b, Str name) {
  for (u32 i = 0; i < b->ntpls; i++) if (str_eq(b->tpls[i].name, name)) return (int)i;
  return -1;
}

/* ---- path helpers ---- */
static Str dir_of(Str path) {
  for (u32 i = path.n; i > 0; i--) if (path.p[i - 1] == '/') return str_make(path.p, i - 1);
  return str_make(path.p, 0);
}
static int path_is_md(Str p) { return p.n >= 3 && memcmp(p.p + p.n - 3, ".md", 3) == 0; }

/* resolvePath tail: process rel segments onto parts[0..nparts) and join.
 * Shared by resolve_path (splits dir itself) and resolve_path_cached
 * (V4: dir split once per page, reused for every link on that page). */
static int resolve_path_tail(Str *parts, u32 nparts, Str rel, char *out, u32 out_cap) {
  u32 pos = 0;
  for (u32 i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Str seg = str_make(rel.p + pos, i - pos);
      pos = i + 1;
      if (seg.n == 0 || str_eq(seg, str_c("."))) continue;
      if (str_eq(seg, str_c(".."))) {
        if (nparts == 0) return 0;
        nparts--;
      } else if (nparts < 64) parts[nparts++] = seg;
    }
  }
  u32 at = 0;
  for (u32 i = 0; i < nparts; i++) {
    if (i) { if (at >= out_cap) return 0; out[at++] = '/'; }
    if (at + parts[i].n > out_cap) return 0;
    memcpy(out + at, parts[i].p, parts[i].n); at += parts[i].n;
  }
  if (at >= out_cap) return 0;
  out[at] = 0;
  return 1;
}

/* resolvePath(dir, rel) into out (NUL-terminated); 0 when it climbs out. */
static int resolve_path(Str dir, Str rel, char *out, u32 out_cap) {
  Str parts[64]; u32 nparts = 0;
  if (dir.n > 0) {
    u32 pos = 0;
    for (u32 i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') { if (nparts < 64) parts[nparts++] = str_make(dir.p + pos, i - pos); pos = i + 1; }
    }
  }
  return resolve_path_tail(parts, nparts, rel, out, out_cap);
}

/* Sources are sorted by path: binary search. */
static Post *by_source(Build *b, Str path) {
  u32 lo = 0, hi = b->nsources;
  while (lo < hi) {
    u32 mid = lo + (hi - lo) / 2;
    int c = cmp_src_fast(b->sources[mid]->path, path);
    if (c == 0) return b->sources[mid];
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return 0;
}
static int is_static(Build *b, Str path) {
  for (u32 i = 0; i < b->nstatics; i++) if (str_eq(b->statics[i].path, path)) return 1;
  return 0;
}

/* ---- the link resolver (markdown's env.link) ---- */
static Str md_link_fn(void *ctx, Str target) {
  Build *b = (Build *)ctx;
  if (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#')) return target;
  if (target.n >= 2) {
    char c0 = target.p[0];
    if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
      u32 i = 1;
      while (i < target.n) {
        char c = target.p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') i++;
        else break;
      }
      if (i < target.n && target.p[i] == ':') return target;
    }
  }
  u32 hash = target.n;
  for (u32 i = 0; i < target.n; i++) if (target.p[i] == '#') { hash = i; break; }
  Str rel = str_make(target.p, hash);
  Str frag = str_make(target.p + hash, target.n - hash);
  char pathbuf[1024];
  int resolved;
  if (b->dir_parts_ok) {
    Str parts[64];
    for (u32 i = 0; i < b->ndir_parts; i++) parts[i] = b->dir_parts[i];
    resolved = resolve_path_tail(parts, b->ndir_parts, rel, pathbuf, sizeof(pathbuf) - 1);
  } else resolved = resolve_path(b->cur_dir, rel, pathbuf, sizeof(pathbuf) - 1);
  if (rel.n > 0 && resolved) {
    Str path = str_c(pathbuf);
    Post *s = by_source(b, path);
    if (s) {
      Buf u; buf_init(&u, s->url.n + frag.n + 4);
      buf_adds(&u, s->url); buf_adds(&u, frag);
      return buf_str(&u);
    }
    if (is_static(b, path)) {
      Buf u; buf_init(&u, b->base.n + path.n + frag.n + 4);
      buf_adds(&u, b->base); buf_addc(&u, '/'); buf_adds(&u, path); buf_adds(&u, frag);
      return buf_str(&u);
    }
  }
  Buf line; buf_init(&line, 64);
  buf_addlit(&line, "broken-link ");
  buf_adds(&line, b->cur_src->path);
  buf_addlit(&line, " -> ");
  buf_adds(&line, target);
  Str l = buf_str(&line);
  if (!strset_has(&b->broken, l)) strset_add(&b->broken, l);
  return target;
}

/* ---- \r\n? -> \n (readSources) ---- */
static Str normalize_crlf(Str s) {
  /* SIMD scan for '\r' (the old scalar byte loop cost a full pass). */
  if (memchr_idx(s.p, s.n, '\r') >= s.n) return s;
  RBuf b; rbuf_init(&b, s.n + 1);
  for (u32 i = 0; i < s.n; i++) {
    if (s.p[i] == '\r') { rbuf_addc(&b, '\n'); if (i + 1 < s.n && s.p[i + 1] == '\n') i++; }
    else rbuf_addc(&b, s.p[i]);
  }
  return str_make(b.p, b.n);
}

static void num_into_buf(Buf *b, long v) {
  char rev[24]; int rn = 0;
  if (v == 0) { buf_addc(b, '0'); return; }
  if (v < 0) { buf_addc(b, '-'); v = -v; }
  while (v) { rev[rn++] = (char)('0' + v % 10); v /= 10; }
  while (rn) buf_addc(b, rev[--rn]);
}

/* ---- the pipeline ---- */
static void build_case(Build *b, Input *in, Out *ans) {
  b->in = in;
  b->ans = ans;
  b->first_entry = 1;
  Str base = in->site.base_url;
  while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;
  b->base = base;
  strset_init(&b->broken, 16);
  out_init(&b->content_out, 65536);
  buf_init(&b->md_scratch, 8192);
  buf_init(&b->rc.var_tmp, 8192);
  b->rc.out_esc = 0;
  out_c(ans, '{');

  /* Parse the templates in code-unit order of name. */
  sort_intpls(in->tpls, in->ntpls);
  b->ntpls = in->ntpls;
  b->tpls = (Tpl *)ralloc(sizeof(Tpl) * (b->ntpls ? b->ntpls : 1));
  b->rc.tpls = b->tpls; b->rc.ntpls = b->ntpls;
  for (u32 i = 0; i < b->ntpls; i++) {
    Tpl *t = &b->tpls[i];
    t->name = in->tpls[i].name;
    t->src = in->tpls[i].src;
    t->failed = 0; t->err = str_make(0, 0);
    t->freevars = 0; t->nfree = 0;
    t->incl = 0; t->nincl = 0;
    t->prerender = 0; t->pre = str_make(0, 0); t->pre_done = 0; t->pre_esc = str_make(0, 0);
    for (int k = 0; k < 4; k++) { t->prog[k] = 0; t->nprog[k] = 0; t->prog_done[k] = 0; }
    Tok *toks; u32 ntoks; Str err = str_make(0, 0);
    if (!tpl_tokenize(in->tpls[i].name, in->tpls[i].src, &toks, &ntoks, &err)) {
      t->failed = 1; t->err = err;
      report_push(b, err);
      continue;
    }
    t->toks = toks; t->ntoks = ntoks;
  }
  tpl_freevars(b->tpls, b->ntpls);
  tpl_prerender_flags(b->tpls, b->ntpls);
  /* Compile every template once up front (CTX_PAGE), exactly like the
   * oracle's upfront compile: a broken template is reported, and a
   * broken layout refuses its sources. */
  for (u32 i = 0; i < b->ntpls; i++) {
    if (b->tpls[i].failed) continue;
    if (!tpl_root_prog(b->tpls, b->ntpls, (int)i, CTX_PAGE)) {
      report_push(b, b->tpls[i].err);
    }
  }

  /* 1. readSources. */
  sort_infiles(in->files, in->nfiles);
  u32 nsrc = 0;
  for (u32 i = 0; i < in->nfiles; i++) if (path_is_md(in->files[i].path)) nsrc++;
  Post **srcs = (Post **)ralloc(sizeof(Post *) * (nsrc ? nsrc : 1));
  InFile *stats = (InFile *)ralloc(sizeof(InFile) * (in->nfiles - nsrc + 1));
  u32 nst = 0, n = 0;
  for (u32 i = 0; i < in->nfiles; i++) {
    if (path_is_md(in->files[i].path)) {
      Post *p = (Post *)ralloc(sizeof(Post));
      memset(p, 0, sizeof(Post));
      p->path = in->files[i].path;
      p->body = normalize_crlf(in->files[i].content);
      srcs[n++] = p;
    } else stats[nst++] = in->files[i];
  }
  b->statics = stats; b->nstatics = nst;

  /* 2. frontMatter. */
  Post **kept = (Post **)ralloc(sizeof(Post *) * (n ? n : 1));
  u32 nk = 0;
  for (u32 i = 0; i < n; i++) {
    Post *s = srcs[i];
    FMResult fm = parse_front_matter(s->body);
    if (!fm.ok) {
      Buf l; buf_init(&l, 64);
      buf_addlit(&l, "refused "); buf_adds(&l, s->path); buf_addlit(&l, ": "); buf_adds(&l, fm.error);
      report_push(b, buf_str(&l));
      continue;
    }
    s->title = fm.data.title;
    s->summary = fm.data.has_summary ? fm.data.summary : str_make(0, 0);
    s->date = fm.data.has_date ? fm.data.date : str_make(0, 0);
    s->body = fm.body;
    s->is_post = s->path.n >= 6 && memcmp(s->path.p, "posts/", 6) == 0;
    s->fm_slug = fm.data.has_slug ? fm.data.slug : str_make(0, 0);
    s->has_fm_slug = fm.data.has_slug;
    s->fm_tags = fm.data.tags; s->nfm_tags = fm.data.ntags; s->has_fm_tags = fm.data.has_tags;
    if (fm.data.has_date) {
      Buf d; buf_init(&d, 32);
      long_date_into(&d, s->date);
      s->date_long = buf_str(&d);
    } else s->date_long = str_make(0, 0);
    long w = words_of(s->body);
    long rm = (w + 199) / 200;
    s->reading_minutes = rm < 1 ? 1 : rm;
    if (s->path.n >= 5 && memcmp(s->path.p, "tags/", 5) == 0) {
      Buf l; buf_init(&l, 64);
      buf_addlit(&l, "refused "); buf_adds(&l, s->path); buf_addlit(&l, ": tags/ is the generator's own");
      report_push(b, buf_str(&l));
      continue;
    }
    if (s->path.n >= 5 && memcmp(s->path.p, "page/", 5) == 0) {
      Buf l; buf_init(&l, 64);
      buf_addlit(&l, "refused "); buf_adds(&l, s->path); buf_addlit(&l, ": page/ is the generator's own");
      report_push(b, buf_str(&l));
      continue;
    }
    if (s->is_post && !fm.data.has_date) {
      Buf l; buf_init(&l, 64);
      buf_addlit(&l, "refused "); buf_adds(&l, s->path); buf_addlit(&l, ": a post needs a date");
      report_push(b, buf_str(&l));
      continue;
    }
    Str layout = fm.data.has_layout ? fm.data.layout : (s->is_post ? str_c("post") : str_c("page"));
    int li = tpl_index(b, layout);
    if (li < 0 || b->tpls[li].failed) {
      Buf l; buf_init(&l, 96);
      buf_addlit(&l, "refused "); buf_adds(&l, s->path); buf_addlit(&l, ": layout \"");
      buf_adds(&l, layout); buf_addlit(&l, "\" is no template that parses");
      report_push(b, buf_str(&l));
      continue;
    }
    s->layout = layout;
    if (fm.data.has_draft && fm.data.draft) {
      Buf l; buf_init(&l, 64);
      buf_addlit(&l, "draft "); buf_adds(&l, s->path);
      report_push(b, buf_str(&l));
      continue;
    }
    kept[nk++] = s;
  }
  b->sources = kept; b->nsources = nk;

  /* 3. permalinks. */
  typedef struct { Str dir; StrSet set; } TakenDir;
  TakenDir *taken_dirs = (TakenDir *)ralloc(sizeof(TakenDir) * 32);
  u32 ntaken = 0, taken_cap = 32;
  for (u32 i = 0; i < nk; i++) {
    Post *s = kept[i];
    Str dir = s->is_post ? str_c("posts") : dir_of(s->path);
    StrSet *set = 0;
    for (u32 k = 0; k < ntaken; k++) if (str_eq(taken_dirs[k].dir, dir)) { set = &taken_dirs[k].set; break; }
    if (!set) {
      if (ntaken == taken_cap) {
        taken_cap *= 2;
        TakenDir *nd = (TakenDir *)ralloc(sizeof(TakenDir) * taken_cap);
        memcpy(nd, taken_dirs, sizeof(TakenDir) * ntaken);
        taken_dirs = nd;
      }
      taken_dirs[ntaken].dir = dir;
      strset_init(&taken_dirs[ntaken].set, 16);
      set = &taken_dirs[ntaken].set;
      ntaken++;
      if (dir.n == 0) {
        strset_add(set, str_c("posts")); strset_add(set, str_c("tags")); strset_add(set, str_c("page"));
      }
    }
    char sb[80];
    Str base_slug;
    if (s->has_fm_slug) base_slug = s->fm_slug;
    else { u32 sl = slugify(sb, s->title); base_slug = str_make(sb, sl); }
    char tmp[80];
    Str slug = md_unique_slug(set, base_slug, tmp);
    s->slug = slug;
    Buf od; buf_init(&od, dir.n + slug.n + 2);
    if (dir.n > 0) { buf_adds(&od, dir); buf_addc(&od, '/'); }
    buf_adds(&od, slug);
    s->out_dir = buf_str(&od);
    Buf u; buf_init(&u, b->base.n + s->out_dir.n + 3);
    buf_adds(&u, b->base); buf_addc(&u, '/'); buf_adds(&u, s->out_dir); buf_addc(&u, '/');
    s->url = buf_str(&u);
  }

  /* 4. markdown is rendered lazily in the layout loop below (step 7):
   * each page's content lives in the shared content cursor only until
   * its own layout is rendered. Rendering all pages up front would
   * alias every page's content to the last page's bytes. Only the
   * excerpt is copied to retained memory: the index/tag cards read it
   * after all pages are done. */

  /* 5. collections: posts newest first, then slug; newer/older. */
  Post **posts = (Post **)ralloc(sizeof(Post *) * (nk ? nk : 1));
  u32 np = 0;
  for (u32 i = 0; i < nk; i++) if (kept[i]->is_post) posts[np++] = kept[i];
  sort_posts(posts, np);
  for (u32 k = 0; k < np; k++) {
    posts[k]->newer = k == 0 ? 0 : posts[k - 1];
    posts[k]->older = k + 1 == np ? 0 : posts[k + 1];
  }
  b->posts = posts; b->nposts = np;

  /* 6. tags. Tags are individually allocated: TagRefs point at them, so
   * the array must be able to grow without moving the structs. */
  Tag **tags = (Tag **)ralloc(sizeof(Tag *) * 64);
  u32 ntags = 0, tags_cap = 64;
  for (u32 pi = 0; pi < np; pi++) {
    Post *p = posts[pi];
    Str *seen = (Str *)ralloc(sizeof(Str) * (p->nfm_tags ? p->nfm_tags : 1));
    u32 nseen = 0;
    TagRef *refs = (TagRef *)ralloc(sizeof(TagRef) * (p->nfm_tags ? p->nfm_tags : 1));
    u32 nrefs = 0;
    for (u32 ti = 0; ti < p->nfm_tags; ti++) {
      char sb[80];
      u32 sl = slugify(sb, p->fm_tags[ti]);
      Str slug = str_make(sb, sl);
      char *keep = (char *)ralloc(sl + 1);
      memcpy(keep, sb, sl);
      slug = str_make(keep, sl);
      int dup = 0;
      for (u32 k = 0; k < nseen; k++) if (str_eq(seen[k], slug)) { dup = 1; break; }
      if (dup) continue;
      seen[nseen++] = slug;
      Tag *t = 0;
      for (u32 k = 0; k < ntags; k++) if (str_eq(tags[k]->slug, slug)) { t = tags[k]; break; }
      if (!t) {
        if (ntags == tags_cap) {
          tags_cap *= 2;
          Tag **nt = (Tag **)ralloc(sizeof(Tag *) * tags_cap);
          memcpy(nt, tags, sizeof(Tag *) * ntags);
          tags = nt;
        }
        t = (Tag *)ralloc(sizeof(Tag));
        tags[ntags++] = t;
        t->name = p->fm_tags[ti];
        t->slug = slug;
        Buf u; buf_init(&u, b->base.n + slug.n + 8);
        buf_adds(&u, b->base); buf_addlit(&u, "/tags/"); buf_adds(&u, slug); buf_addc(&u, '/');
        t->url = buf_str(&u);
        t->posts = (Post **)ralloc(sizeof(Post *) * 64);
        t->nposts = 0;
      }
      if (t->nposts % 64 == 0 && t->nposts > 0) {
        Post **nv = (Post **)ralloc(sizeof(Post *) * (t->nposts + 64));
        memcpy(nv, t->posts, sizeof(Post *) * t->nposts);
        t->posts = nv;
      }
      t->posts[t->nposts++] = p;
      refs[nrefs].name = t->name;
      refs[nrefs].slug = slug;
      refs[nrefs].url = t->url;
      refs[nrefs].tag = t;
      nrefs++;
    }
    p->tags = refs; p->ntags = nrefs;
  }
  for (u32 i = 0; i < nk; i++) {
    Post *s = kept[i];
    if (s->is_post) continue;
    Str *seen = (Str *)ralloc(sizeof(Str) * (s->nfm_tags ? s->nfm_tags : 1));
    u32 nseen = 0;
    TagRef *refs = (TagRef *)ralloc(sizeof(TagRef) * (s->nfm_tags ? s->nfm_tags : 1));
    u32 nrefs = 0;
    for (u32 ti = 0; ti < s->nfm_tags; ti++) {
      char sb[80];
      u32 sl = slugify(sb, s->fm_tags[ti]);
      Str slug = str_make(sb, sl);
      char *keep = (char *)ralloc(sl + 1);
      memcpy(keep, sb, sl);
      slug = str_make(keep, sl);
      int dup = 0;
      for (u32 k = 0; k < nseen; k++) if (str_eq(seen[k], slug)) { dup = 1; break; }
      if (dup) continue;
      seen[nseen++] = slug;
      Tag *t = 0;
      for (u32 k = 0; k < ntags; k++) if (str_eq(tags[k]->slug, slug)) { t = tags[k]; break; }
      refs[nrefs].name = t ? t->name : s->fm_tags[ti];
      refs[nrefs].slug = slug;
      refs[nrefs].url = t ? t->url : str_make(0, 0);
      refs[nrefs].tag = t;
      nrefs++;
    }
    s->tags = refs; s->ntags = nrefs;
  }
  Tag **tptrs = (Tag **)ralloc(sizeof(Tag *) * (ntags ? ntags : 1));
  for (u32 i = 0; i < ntags; i++) tptrs[i] = tags[i];
  sort_tagptrs(tptrs, ntags);
  b->tags = tags; b->ntags = ntags;
  TagSum *all = (TagSum *)ralloc(sizeof(TagSum) * (ntags ? ntags : 1));
  for (u32 i = 0; i < ntags; i++) {
    all[i].name = tptrs[i]->name; all[i].slug = tptrs[i]->slug;
    all[i].url = tptrs[i]->url; all[i].count = (long)tptrs[i]->nposts;
  }
  b->all_tags = all;

  /* 7. layouts. */
  SiteInfo site; site.title = in->site.title; site.base_url = b->base;
  Val site_v = v_obj(O_SITE, &site);
  Val tags_v;
  {
    Val *items = (Val *)ralloc(sizeof(Val) * (ntags ? ntags : 1));
    for (u32 i = 0; i < ntags; i++) items[i] = v_obj(O_TAGSUM, &all[i]);
    tags_v = v_arr(items, ntags);
  }
  Val recent_v;
  {
    u32 nr = in->site.recent < 0 ? 0 : (u32)in->site.recent;
    if (nr > np) nr = np;
    recent_v = cards_of(posts, nr);
  }
  /* Pre-render the constant templates once per case, in dependency
   * order: a template's pre string is only valid once every prerender
   * template it transitively includes is rendered. (Rendering in name
   * order is wrong: a header that includes nav would splice nav's
   * still-empty pre string.) The fixpoint loop terminates because the
   * include graph is a DAG in practice. */
  {
    u8 *vis = (u8 *)ralloc(b->ntpls ? b->ntpls : 1);
    int progress = 1;
    while (progress) {
      progress = 0;
      for (u32 i = 0; i < b->ntpls; i++) {
        Tpl *t = &b->tpls[i];
        if (!t->prerender || t->failed || t->pre_done) continue;
        memset(vis, 0, b->ntpls ? b->ntpls : 1);
        /* Transitive readiness check (iterative DFS over includes). */
        int ready = 1;
        u32 stack[64]; int sp = 0;
        for (u32 k = 0; k < t->nincl && sp < 64; k++) stack[sp++] = t->incl[k];
        while (sp > 0 && ready) {
          u32 idx = stack[--sp];
          if (vis[idx]) continue;
          vis[idx] = 1;
          Tpl *u = &b->tpls[idx];
          if (u->prerender && !u->failed && !u->pre_done) { ready = 0; break; }
          for (u32 k = 0; k < u->nincl && sp < 64; k++) stack[sp++] = u->incl[k];
        }
        if (!ready) continue;
        t->pre = render_pre_str(&b->rc, (int)i, site_v, tags_v, recent_v);
        t->pre_done = 1;
        progress = 1;
      }
    }
  }
  /* Every page through its layout (markdown rendered just in time). */
  for (u32 i = 0; i < nk; i++) {
    Post *s = kept[i];
    int li = tpl_index(b, s->layout);
    if (li < 0 || b->tpls[li].failed) continue;
    b->cur_src = s;
    b->cur_dir = dir_of(s->path);
    { /* V4: split cur_dir once per page (same split as resolve_path). */
      b->ndir_parts = 0; b->dir_parts_ok = 1;
      if (b->cur_dir.n > 0) {
        u32 pos = 0;
        for (u32 i = 0; i <= b->cur_dir.n; i++) {
          if (i == b->cur_dir.n || b->cur_dir.p[i] == '/') {
            if (b->ndir_parts >= 16) { b->dir_parts_ok = 0; break; }
            b->dir_parts[b->ndir_parts++] = str_make(b->cur_dir.p + pos, i - pos);
            pos = i + 1;
          }
        }
      }
    }
    b->content_out.cur = b->content_out.p; /* reset the content cursor */
    MdResult md = md_render(s->body, md_link_fn, b, &b->content_out, &b->md_scratch);
    s->content = out_str(&b->content_out);
    s->toc = md.toc; s->ntoc = md.ntoc;
    /* The excerpt outlives this page's render (index/tag cards read it
     * later), so copy it to retained memory; the content and toc are
     * consumed by this page's layout right below. */
    if (md.has_excerpt && md.excerpt.n > 0) {
      char *ep = (char *)ralloc(md.excerpt.n);
      memcpy(ep, md.excerpt.p, md.excerpt.n);
      s->excerpt = str_make(ep, md.excerpt.n);
    } else s->excerpt = str_make(0, 0);
    Val root[5];
    root[0] = site_v; root[1] = tags_v; root[2] = recent_v;
    root[3] = v_obj(O_POST, s); root[4] = v_undef();
    Buf p; buf_init(&p, s->out_dir.n + 12);
    buf_adds(&p, s->out_dir); buf_addlit(&p, "/index.html");
    emit_begin(b, buf_str(&p));
    render_tpl(&b->rc, li, CTX_PAGE, root);
    emit_end(b);
  }
  /* The index pages. */
  long per = in->site.per_page < 1 ? 1 : in->site.per_page;
  long pages = np == 0 ? 1 : (long)((np + (u32)per - 1) / (u32)per);
  if (pages < 1) pages = 1;
  int idx_tpl = tpl_index(b, str_c("index"));
  for (long k = 1; k <= pages; k++) {
    u32 from = (u32)((k - 1) * per);
    u32 to = (u32)(k * per);
    if (to > np) to = np;
    if (from > np) from = np;
    Val posts_v = cards_of(posts + from, to - from);
    PagerVal *pg = (PagerVal *)ralloc(sizeof(PagerVal));
    pg->number = k; pg->total = pages;
    Buf u1; buf_init(&u1, 64);
    if (k == 1) { buf_adds(&u1, b->base); buf_addc(&u1, '/'); }
    else {
      buf_adds(&u1, b->base); buf_addlit(&u1, "/page/");
      num_into_buf(&u1, k); buf_addc(&u1, '/');
    }
    pg->url = buf_str(&u1);
    if (k == 1) pg->newer_url = str_make(0, 0);
    else {
      Buf u2; buf_init(&u2, 64);
      if (k - 1 == 1) { buf_adds(&u2, b->base); buf_addc(&u2, '/'); }
      else { buf_adds(&u2, b->base); buf_addlit(&u2, "/page/"); num_into_buf(&u2, k - 1); buf_addc(&u2, '/'); }
      pg->newer_url = buf_str(&u2);
    }
    if (k == pages) pg->older_url = str_make(0, 0);
    else {
      Buf u3; buf_init(&u3, 64);
      buf_adds(&u3, b->base); buf_addlit(&u3, "/page/"); num_into_buf(&u3, k + 1); buf_addc(&u3, '/');
      pg->older_url = buf_str(&u3);
    }
    if (idx_tpl < 0 || b->tpls[idx_tpl].failed) continue;
    Val root[5];
    root[0] = site_v; root[1] = tags_v; root[2] = recent_v;
    root[3] = posts_v; root[4] = v_obj(O_PAGER, pg);
    if (k == 1) emit_begin(b, str_c("index.html"));
    else {
      Buf p; buf_init(&p, 32);
      buf_addlit(&p, "page/"); num_into_buf(&p, k); buf_addlit(&p, "/index.html");
      emit_begin(b, buf_str(&p));
    }
    render_tpl(&b->rc, idx_tpl, CTX_POSTS, root);
    emit_end(b);
  }
  /* The tag pages. */
  int tag_tpl = tpl_index(b, str_c("tag"));
  for (u32 i = 0; i < ntags; i++) {
    Tag *t = tptrs[i];
    if (tag_tpl < 0 || b->tpls[tag_tpl].failed) break;
    Val root[5];
    root[0] = site_v; root[1] = tags_v; root[2] = recent_v;
    root[3] = v_obj(O_TAG, t); root[4] = v_undef();
    Buf p; buf_init(&p, t->slug.n + 16);
    buf_addlit(&p, "tags/"); buf_adds(&p, t->slug); buf_addlit(&p, "/index.html");
    emit_begin(b, buf_str(&p));
    render_tpl(&b->rc, tag_tpl, CTX_TAG, root);
    emit_end(b);
  }
  /* The tags index. */
  int tags_tpl = tpl_index(b, str_c("tags"));
  if (tags_tpl >= 0 && !b->tpls[tags_tpl].failed) {
    Val root[5];
    root[0] = site_v; root[1] = tags_v; root[2] = recent_v;
    root[3] = v_undef(); root[4] = v_undef();
    emit_begin(b, str_c("tags/index.html"));
    render_tpl(&b->rc, tags_tpl, CTX_TAGS, root);
    emit_end(b);
  }
  /* The statics. */
  for (u32 i = 0; i < nst; i++) {
    if (out_has(b, stats[i].path)) {
      Buf l; buf_init(&l, 64);
      buf_addlit(&l, "shadowed "); buf_adds(&l, stats[i].path);
      buf_addlit(&l, ": a built page has its path");
      report_push(b, buf_str(&l));
    } else emit_entry(b, stats[i].path, stats[i].content);
  }

  /* 8. The report. */
  {
    u32 nb = b->broken.n;
    u32 total = b->nreport + nb;
    Str *lines = (Str *)ralloc(sizeof(Str) * (total ? total : 1));
    for (u32 i = 0; i < b->nreport; i++) lines[i] = b->report[i];
    for (u32 i = 0; i < nb; i++) lines[b->nreport + i] = b->broken.v[i];
    sort_strs(lines, total);
    Buf rep; buf_init(&rep, 4096);
    for (u32 i = 0; i < total; i++) { buf_adds(&rep, lines[i]); buf_addc(&rep, '\n'); }
    u32 npages = 0;
    for (u32 i = 0; i < nk; i++) if (!kept[i]->is_post) npages++;
    buf_addlit(&rep, "built ");
    num_into_buf(&rep, (long)npages);
    buf_addlit(&rep, " pages, ");
    num_into_buf(&rep, (long)np);
    buf_addlit(&rep, " posts, ");
    num_into_buf(&rep, (long)ntags);
    buf_addlit(&rep, " tags and ");
    num_into_buf(&rep, (long)nst);
    buf_addlit(&rep, " static files\n");
    emit_entry(b, str_c("build-report.txt"), buf_str(&rep));
  }
  out_c(ans, '}');
}

#endif
