/* Templates: makefun-site's dialect (lib/template.js), compiled to a flat
 * instruction list.
 *
 * Fused design (vs the V6 interpreter):
 *  - Scope slots resolved at COMPILE time. The root frame has fixed slots
 *    (site=0, tags=1, recent=2, page/posts/tag=3, pager=4); each for-frame
 *    holds [loopvar, loop]. A path root resolves to (frame, slot) using the
 *    compiler's for-stack, so rendering never scans bindings by name.
 *  - Includes are INLINED at compile time (the include graph is small,
 *    depth-capped at 8 exactly like the runtime depth check), so there is
 *    no per-include dispatch, fingerprinting or sub-buffer.
 *  - Templates whose transitive free variables are only {site, tags,
 *    recent} are PRE-RENDERED once per case and spliced in as OP_SPLICE
 *    (safe only when no for-loop anywhere binds a root name; checked
 *    globally, otherwise pre-rendering is disabled).
 *  - Root programs are compiled lazily per (template, render context):
 *    PAGE (slot3=page), POSTS (slot3=posts, slot4=pager), TAG (slot3=tag),
 *    TAGS (no slot3/4). A path root that has no binding in the context
 *    resolves to undef, exactly like a scope miss.
 */
#ifndef SG_TPL_H
#define SG_TPL_H

#include "rt.h"
#include "text.h"
#include "json.h"

/* ---- name interning (global, one table for the run) ---- */
#define INTERN_TAB_N 4096
typedef struct { Str s; u32 id; u8 used; } InternEnt;
static InternEnt INTERN_TAB[INTERN_TAB_N];
static u32 INTERN_NEXT = 1;
static u32 intern(Str s) {
  u64 h = hash_str(s);
  u32 slot = (u32)(h & (INTERN_TAB_N - 1));
  for (;;) {
    InternEnt *e = &INTERN_TAB[slot];
    if (!e->used) { e->used = 1; e->s = s; e->id = INTERN_NEXT++; return e->id; }
    if (str_eq(e->s, s)) return e->id;
    slot = (slot + 1) & (INTERN_TAB_N - 1);
  }
}
static u32 intern_c(const char *c) { return intern(str_c(c)); }

static u32 ID_site, ID_tags, ID_recent, ID_page, ID_posts, ID_pager, ID_tag, ID_loop;
static u32 ID_title, ID_url, ID_date, ID_date_long, ID_summary, ID_excerpt, ID_reading_minutes;
static u32 ID_slug, ID_content, ID_toc, ID_newer, ID_older, ID_is_post;
static u32 ID_name, ID_count, ID_number, ID_total, ID_newer_url, ID_older_url;
static u32 ID_level, ID_id, ID_html, ID_index, ID_first, ID_last, ID_length, ID_base_url;

static void tpl_intern_names(void) {
  ID_site = intern_c("site"); ID_tags = intern_c("tags"); ID_recent = intern_c("recent");
  ID_page = intern_c("page"); ID_posts = intern_c("posts"); ID_pager = intern_c("pager");
  ID_tag = intern_c("tag"); ID_loop = intern_c("loop");
  ID_title = intern_c("title"); ID_url = intern_c("url"); ID_date = intern_c("date");
  ID_date_long = intern_c("date_long"); ID_summary = intern_c("summary");
  ID_excerpt = intern_c("excerpt"); ID_reading_minutes = intern_c("reading_minutes");
  ID_slug = intern_c("slug"); ID_content = intern_c("content"); ID_toc = intern_c("toc");
  ID_newer = intern_c("newer"); ID_older = intern_c("older"); ID_is_post = intern_c("is_post");
  ID_name = intern_c("name"); ID_count = intern_c("count"); ID_number = intern_c("number");
  ID_total = intern_c("total"); ID_newer_url = intern_c("newer_url"); ID_older_url = intern_c("older_url");
  ID_level = intern_c("level"); ID_id = intern_c("id"); ID_html = intern_c("html");
  ID_index = intern_c("index"); ID_first = intern_c("first"); ID_last = intern_c("last");
  ID_length = intern_c("length"); ID_base_url = intern_c("base_url");
}

/* ---- instructions ---- */
enum { OP_TEXT, OP_VAR, OP_RAW, OP_IF, OP_JMP, OP_FOR, OP_NEXT, OP_SPLICE, OP_END };
enum { CTX_PAGE = 0, CTX_POSTS = 1, CTX_TAG = 2, CTX_TAGS = 3 };

typedef struct {
  u8 op, flags;      /* flags: OP_IF bit0 = "not" */
  u8 fframe, fslot;  /* resolved root (OP_VAR/OP_RAW/OP_IF/OP_FOR) */
  u8 undef;          /* path root has no binding -> always undef */
  u32 a, b;          /* OP_IF: a=else_pc, b=end_pc; OP_JMP: a=target;
                        OP_FOR: a=end_pc, b=loop-var id; OP_NEXT: a=for_pc;
                        OP_SPLICE: a=template index */
  Str text;          /* OP_TEXT */
  Str text_esc;      /* OP_TEXT: JSON-escaped copy (fused emit: plain copy) */
  u32 *segs; u32 nsegs; /* field walk after the root segment */
} Ins;

typedef struct {
  u8 kind; Str text; u32 *segs; u32 nsegs;
} Tok;
enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG };

typedef struct {
  Str name;
  Str src;
  Tok *toks; u32 ntoks;
  int failed; Str err;
  u32 *freevars; u32 nfree;
  u32 *incl; u32 nincl;
  int prerender;
  Ins *prog[4]; u32 nprog[4]; int prog_done[4];
  Str pre; int pre_done;
  Str pre_esc; /* JSON-escaped copy of pre (fused emit: plain copy) */
} Tpl;

/* ---- path parsing ---- */
static void json_quote_into(Buf *b, Str s) {
  buf_addc(b, '"');
  json_escape_into(b, s);
  buf_addc(b, '"');
}

static int path_parse(Str text, u32 **segs_out, u32 *n_out, Str tpl_name, Str *err_out) {
  Str t = js_trim(text);
  int ok = t.n > 0;
  if (ok) {
    char c = t.p[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) ok = 0;
    for (u32 i = 1; ok && i < t.n; i++) {
      char d = t.p[i];
      if (!((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || (d >= '0' && d <= '9') || d == '_' || d == '.')) ok = 0;
    }
    for (u32 i = 0; ok && i < t.n; i++) {
      if (t.p[i] == '.') {
        if (i == 0 || i + 1 >= t.n) ok = 0;
        else {
          char d = t.p[i + 1];
          if (!((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || d == '_')) ok = 0;
        }
        if (i > 0 && t.p[i - 1] == '.') ok = 0;
      }
    }
  }
  if (!ok) {
    Buf b; buf_init(&b, 96);
    buf_addlit(&b, "template \"");
    buf_adds(&b, tpl_name);
    buf_addlit(&b, "\": ");
    json_quote_into(&b, t);
    buf_addlit(&b, " is not a path");
    *err_out = buf_str(&b);
    return 0;
  }
  u32 cnt = 1;
  for (u32 i = 0; i < t.n; i++) if (t.p[i] == '.') cnt++;
  u32 *segs = (u32 *)ralloc(sizeof(u32) * cnt);
  u32 n = 0, pos = 0;
  for (u32 i = 0; i <= t.n; i++) {
    if (i == t.n || t.p[i] == '.') {
      segs[n++] = intern(str_make(t.p + pos, i - pos));
      pos = i + 1;
    }
  }
  *segs_out = segs; *n_out = n;
  return 1;
}

/* ---- tokenizer ---- */
static int tpl_tokenize(Str name, Str src, Tok **toks_out, u32 *ntoks_out, Str *err_out) {
  u32 cap = 32, n = 0;
  Tok *toks = (Tok *)ralloc(sizeof(Tok) * cap);
  Buf text; buf_init(&text, 256);
  u32 i = 0;
  while (i < src.n) {
    u32 open = src.n;
    for (u32 k = i; k < src.n; k++) if (src.p[k] == '{') { open = k; break; }
    if (open == src.n || open == src.n - 1) { buf_add(&text, src.p + i, src.n - i); break; }
    char next = src.p[open + 1];
    if (next != '{' && next != '%') { buf_add(&text, src.p + i, open + 1 - i); i = open + 1; continue; }
    buf_add(&text, src.p + i, open - i);
    if (next == '{') {
      int raw = open + 2 < src.n && src.p[open + 2] == '{';
      const char *close_s = raw ? "}}}" : "}}";
      u32 cl = raw ? 3 : 2;
      u32 close = src.n;
      for (u32 k = open + cl; k + cl <= src.n; k++) if (memcmp(src.p + k, close_s, cl) == 0) { close = k; break; }
      if (close == src.n) {
        Buf b; buf_init(&b, 96);
        buf_addlit(&b, "template \""); buf_adds(&b, name);
        buf_addlit(&b, "\": a "); buf_addlit(&b, raw ? "{{{" : "{{");
        buf_addlit(&b, " is not closed");
        *err_out = buf_str(&b);
        return 0;
      }
      if (text.n > 0) {
        if (n == cap) { cap *= 2; Tok *nt = (Tok *)ralloc(sizeof(Tok) * cap); memcpy(nt, toks, sizeof(Tok) * n); toks = nt; }
        toks[n].kind = TK_TEXT; toks[n].text = buf_str(&text); n++;
        buf_init(&text, 256);
      }
      u32 *segs; u32 nsegs;
      if (!path_parse(str_make(src.p + open + cl, close - (open + cl)), &segs, &nsegs, name, err_out)) return 0;
      if (n == cap) { cap *= 2; Tok *nt = (Tok *)ralloc(sizeof(Tok) * cap); memcpy(nt, toks, sizeof(Tok) * n); toks = nt; }
      toks[n].kind = raw ? TK_RAW : TK_VAR; toks[n].segs = segs; toks[n].nsegs = nsegs; n++;
      i = close + cl;
      continue;
    }
    u32 close = src.n;
    for (u32 k = open + 2; k + 2 <= src.n; k++) if (src.p[k] == '%' && src.p[k + 1] == '}') { close = k; break; }
    if (close == src.n) {
      Buf b; buf_init(&b, 96);
      buf_addlit(&b, "template \""); buf_adds(&b, name);
      buf_addlit(&b, "\": a {% is not closed");
      *err_out = buf_str(&b);
      return 0;
    }
    u32 end = close + 2;
    u32 line_start = open;
    while (line_start > 0 && src.p[line_start - 1] != '\n') line_start--;
    u32 nl = src.n;
    for (u32 k = end; k < src.n; k++) if (src.p[k] == '\n') { nl = k; break; }
    u32 line_end = nl;
    int standalone = 1;
    for (u32 k = line_start; k < open; k++) if (!is_space_tab(src.p[k])) standalone = 0;
    for (u32 k = end; k < line_end; k++) if (!is_space_tab(src.p[k])) standalone = 0;
    if (standalone) {
      u32 drop = open - line_start;
      if (drop > text.n) drop = text.n;
      text.n -= drop;
    }
    if (text.n > 0) {
      if (n == cap) { cap *= 2; Tok *nt = (Tok *)ralloc(sizeof(Tok) * cap); memcpy(nt, toks, sizeof(Tok) * n); toks = nt; }
      toks[n].kind = TK_TEXT; toks[n].text = buf_str(&text); n++;
      buf_init(&text, 256);
    }
    if (n == cap) { cap *= 2; Tok *nt = (Tok *)ralloc(sizeof(Tok) * cap); memcpy(nt, toks, sizeof(Tok) * n); toks = nt; }
    toks[n].kind = TK_TAG; toks[n].text = js_trim(str_make(src.p + open + 2, close - (open + 2))); n++;
    i = standalone ? (nl == src.n ? src.n : nl + 1) : end;
  }
  if (text.n > 0) {
    if (n == cap) { cap *= 2; Tok *nt = (Tok *)ralloc(sizeof(Tok) * cap); memcpy(nt, toks, sizeof(Tok) * n); toks = nt; }
    toks[n].kind = TK_TEXT; toks[n].text = buf_str(&text); n++;
  }
  *toks_out = toks; *ntoks_out = n;
  return 1;
}

/* ---- tag classification (shared by the freevar scan and the compiler) ---- */
enum { TG_FOR, TG_IF, TG_ELSE, TG_ENDFOR, TG_ENDIF, TG_INCLUDE, TG_BAD };
typedef struct {
  int kind;
  u32 var;        /* TG_FOR: loop-var id */
  u32 *segs; u32 nsegs; /* TG_FOR/TG_IF: the path */
  int not;        /* TG_IF: "not" */
  Str inc_name;   /* TG_INCLUDE */
} TagInfo;

static int tag_parse(Str tag, Str tpl_name, TagInfo *ti, Str *err_out) {
  ti->kind = TG_BAD; ti->var = 0; ti->segs = 0; ti->nsegs = 0; ti->not = 0; ti->inc_name = str_make(0, 0);
  if (tag.n >= 4 && memcmp(tag.p, "for ", 4) == 0) {
    u32 p = 4, v0 = p;
    while (p < tag.n && (is_alnum_c(tag.p[p]) || tag.p[p] == '_')) p++;
    int ok = p > v0 && (is_upper(tag.p[v0]) || is_lower(tag.p[v0]) || tag.p[v0] == '_') &&
             p + 4 <= tag.n && memcmp(tag.p + p, " in ", 4) == 0;
    u32 path0 = p + 4;
    if (ok) {
      for (u32 k = path0; k < tag.n; k++) if (tag.p[k] == ' ' || tag.p[k] == '\t') ok = 0;
      if (ok && path0 >= tag.n) ok = 0;
    }
    if (!ok) return 0;
    if (!path_parse(str_make(tag.p + path0, tag.n - path0), &ti->segs, &ti->nsegs, tpl_name, err_out)) return 0;
    ti->kind = TG_FOR;
    ti->var = intern(str_make(tag.p + v0, p - v0));
    return 1;
  }
  if (tag.n >= 3 && memcmp(tag.p, "if ", 3) == 0) {
    u32 p = 3; int not = 0;
    if (p + 4 <= tag.n && memcmp(tag.p + p, "not ", 4) == 0) { not = 1; p += 4; }
    int ok = p < tag.n;
    for (u32 k = p; ok && k < tag.n; k++) if (tag.p[k] == ' ' || tag.p[k] == '\t') ok = 0;
    if (!ok) return 0;
    if (!path_parse(str_make(tag.p + p, tag.n - p), &ti->segs, &ti->nsegs, tpl_name, err_out)) return 0;
    ti->kind = TG_IF; ti->not = not;
    return 1;
  }
  if (str_eq(tag, str_c("else"))) { ti->kind = TG_ELSE; return 1; }
  if (str_eq(tag, str_c("endfor"))) { ti->kind = TG_ENDFOR; return 1; }
  if (str_eq(tag, str_c("endif"))) { ti->kind = TG_ENDIF; return 1; }
  if (tag.n >= 9 && memcmp(tag.p, "include ", 8) == 0) {
    int ok = tag.n >= 10 && tag.p[8] == '"' && tag.p[tag.n - 1] == '"';
    if (ok) for (u32 k = 9; k + 1 < tag.n; k++) {
      char c = tag.p[k];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
    }
    if (!ok || tag.n == 10) return 0;
    ti->kind = TG_INCLUDE;
    ti->inc_name = str_make(tag.p + 9, tag.n - 10);
    return 1;
  }
  return 0;
}

/* ---- free-variable analysis (from tokens, lenient: tag errors are left
 * for the compiler, which reports them with the template's name) ---- */
static int tpl_index_of(Tpl *tpls, u32 n, Str name) {
  for (u32 i = 0; i < n; i++) if (str_eq(tpls[i].name, name)) return (int)i;
  return -1;
}

static void tpl_freevars(Tpl *tpls, u32 n) {
  for (u32 ti = 0; ti < n; ti++) {
    Tpl *t = &tpls[ti];
    if (t->failed) { t->freevars = 0; t->nfree = 0; t->incl = 0; t->nincl = 0; continue; }
    u32 cap = 64, cnt = 0;
    u32 *freev = (u32 *)ralloc(sizeof(u32) * cap);
    u32 icap = 16, ni = 0;
    u32 *incl = (u32 *)ralloc(sizeof(u32) * icap);
    u32 bound[32]; int nbound = 0;
    for (u32 k = 0; k < t->ntoks; k++) {
      Tok *tk = &t->toks[k];
      if (tk->kind == TK_VAR || tk->kind == TK_RAW) {
        if (tk->nsegs > 0) {
          u32 r = tk->segs[0];
          int isbound = 0;
          for (int q = 0; q < nbound; q++) if (bound[q] == r) isbound = 1;
          if (!isbound) {
            int dup = 0; for (u32 m = 0; m < cnt; m++) if (freev[m] == r) dup = 1;
            if (!dup) { if (cnt == cap) { cap *= 2; u32 *nf = (u32 *)ralloc(sizeof(u32) * cap); memcpy(nf, freev, sizeof(u32) * cnt); freev = nf; } freev[cnt++] = r; }
          }
        }
        continue;
      }
      if (tk->kind != TK_TAG) continue;
      TagInfo inf; Str err = str_make(0, 0);
      if (!tag_parse(tk->text, t->name, &inf, &err)) continue; /* compiler reports */
      if (inf.kind == TG_FOR) {
        if (inf.nsegs > 0) {
          u32 r = inf.segs[0];
          int isbound = 0;
          for (int q = 0; q < nbound; q++) if (bound[q] == r) isbound = 1;
          if (!isbound) {
            int dup = 0; for (u32 m = 0; m < cnt; m++) if (freev[m] == r) dup = 1;
            if (!dup) { if (cnt == cap) { cap *= 2; u32 *nf = (u32 *)ralloc(sizeof(u32) * cap); memcpy(nf, freev, sizeof(u32) * cnt); freev = nf; } freev[cnt++] = r; }
          }
        }
        if (nbound + 2 <= 32) { bound[nbound++] = inf.var; bound[nbound++] = ID_loop; }
        continue;
      }
      if (inf.kind == TG_ENDFOR) { if (nbound >= 2) nbound -= 2; continue; }
      if (inf.kind == TG_IF) {
        if (inf.nsegs > 0) {
          u32 r = inf.segs[0];
          int isbound = 0;
          for (int q = 0; q < nbound; q++) if (bound[q] == r) isbound = 1;
          if (!isbound) {
            int dup = 0; for (u32 m = 0; m < cnt; m++) if (freev[m] == r) dup = 1;
            if (!dup) { if (cnt == cap) { cap *= 2; u32 *nf = (u32 *)ralloc(sizeof(u32) * cap); memcpy(nf, freev, sizeof(u32) * cnt); freev = nf; } freev[cnt++] = r; }
          }
        }
        continue;
      }
      if (inf.kind == TG_INCLUDE) {
        int idx = tpl_index_of(tpls, n, inf.inc_name);
        if (idx >= 0) {
          int dup = 0; for (u32 m = 0; m < ni; m++) if (incl[m] == (u32)idx) dup = 1;
          if (!dup) { if (ni == icap) { icap *= 2; u32 *n2 = (u32 *)ralloc(sizeof(u32) * icap); memcpy(n2, incl, sizeof(u32) * ni); incl = n2; } incl[ni++] = (u32)idx; }
        }
        continue;
      }
    }
    for (u32 a = 1; a < cnt; a++) { u32 x = freev[a], j = a; while (j > 0 && freev[j - 1] > x) { freev[j] = freev[j - 1]; j--; } freev[j] = x; }
    t->freevars = freev; t->nfree = cnt;
    t->incl = incl; t->nincl = ni;
  }
  /* Transitive closure over the include graph. */
  for (int round = 0; round < 32; round++) {
    int changed = 0;
    for (u32 i = 0; i < n; i++) {
      Tpl *t = &tpls[i];
      if (t->failed) continue;
      for (u32 k = 0; k < t->nincl; k++) {
        Tpl *u = &tpls[t->incl[k]];
        if (u->failed) continue;
        for (u32 j = 0; j < u->nfree; j++) {
          u32 name = u->freevars[j];
          int dup = 0;
          for (u32 m = 0; m < t->nfree; m++) if (t->freevars[m] == name) { dup = 1; break; }
          if (!dup && t->nfree < 64) { t->freevars[t->nfree++] = name; changed = 1; }
        }
      }
    }
    if (!changed) break;
  }
  for (u32 i = 0; i < n; i++) {
    Tpl *t = &tpls[i];
    for (u32 a = 1; a < t->nfree; a++) { u32 x = t->freevars[a], j = a; while (j > 0 && t->freevars[j - 1] > x) { t->freevars[j] = t->freevars[j - 1]; j--; } t->freevars[j] = x; }
  }
}

/* Pre-render flags + the global shadowing check. Returns 1 when
 * pre-rendering is safe (no for-loop anywhere binds a root name). */
static int tpl_prerender_flags(Tpl *tpls, u32 n) {
  int safe = 1;
  for (u32 i = 0; i < n && safe; i++) {
    if (tpls[i].failed) continue;
    for (u32 k = 0; k < tpls[i].ntoks; k++) {
      Tok *tk = &tpls[i].toks[k];
      if (tk->kind != TK_TAG) continue;
      TagInfo inf; Str err = str_make(0, 0);
      if (!tag_parse(tk->text, tpls[i].name, &inf, &err)) continue;
      if (inf.kind == TG_FOR) {
        u32 v = inf.var;
        if (v == ID_site || v == ID_tags || v == ID_recent || v == ID_page ||
            v == ID_posts || v == ID_pager || v == ID_tag || v == ID_loop) { safe = 0; break; }
      }
    }
  }
  for (u32 i = 0; i < n; i++) {
    Tpl *t = &tpls[i];
    t->prerender = 0;
    if (t->failed || !safe) continue;
    int ok = 1;
    for (u32 k = 0; k < t->nfree; k++) {
      u32 v = t->freevars[k];
      if (v != ID_site && v != ID_tags && v != ID_recent) { ok = 0; break; }
    }
    t->prerender = ok;
  }
  return safe;
}

/* ---- the compiler (inlining + slot resolution) ---- */
typedef struct {
  Tpl *tpls; u32 ntpls;
  int ctx;                 /* CTX_* render context */
  Ins *ins; u32 nins, cap; /* output list being built (scratch) */
  u32 fs_name[64]; u8 fs_frame[64]; u8 fs_slot[64]; int nfs; /* for-stack */
  int nfor;                /* enclosing for count (= innermost for frame index) */
  int depth;               /* include depth of the template being emitted */
  /* compile stack for for/if patching (shared across inlining: frames
   * opened by an enclosing template sit below sp0) */
  struct { u8 kind; u32 ins; u8 has_else; u32 jmp; Str name; } st[64];
  int sp;
} CCtx;

static void cc_emit_ins(CCtx *c, Ins *in) {
  if (c->nins == c->cap) {
    c->cap = c->cap ? c->cap * 2 : 64;
    Ins *ni = (Ins *)salloc(sizeof(Ins) * c->cap);
    if (c->ins) memcpy(ni, c->ins, sizeof(Ins) * c->nins);
    c->ins = ni;
  }
  c->ins[c->nins++] = *in;
}

static int cc_resolve(CCtx *c, u32 root, u8 *frame, u8 *slot) {
  for (int i = c->nfs - 1; i >= 0; i--) {
    if (c->fs_name[i] == root) { *frame = c->fs_frame[i]; *slot = c->fs_slot[i]; return 1; }
  }
  if (root == ID_site) { *frame = 0; *slot = 0; return 1; }
  if (root == ID_tags) { *frame = 0; *slot = 1; return 1; }
  if (root == ID_recent) { *frame = 0; *slot = 2; return 1; }
  if (root == ID_page && c->ctx == CTX_PAGE) { *frame = 0; *slot = 3; return 1; }
  if (root == ID_posts && c->ctx == CTX_POSTS) { *frame = 0; *slot = 3; return 1; }
  if (root == ID_tag && c->ctx == CTX_TAG) { *frame = 0; *slot = 3; return 1; }
  if (root == ID_pager && c->ctx == CTX_POSTS) { *frame = 0; *slot = 4; return 1; }
  return 0;
}

/* Emit one template's instructions into c->ins (inlined). Returns 1 when
 * emitted, 0 when skipped (failed/missing/too deep), -1 when the template
 * itself is broken (bad tag / unbalanced) -- the caller rewinds. */
static int cc_emit_tpl(CCtx *c, int tidx);

static int cc_emit_tokens(CCtx *c, Tpl *t, Str *err_out) {
  /* sp0: the compile-stack depth on entry. Frames below sp0 belong to an
   * enclosing template (inlining): an endfor/endif here must not pop
   * them, and the "not closed" check only counts frames THIS template
   * opened. */
  int sp0 = c->sp;
  for (u32 k = 0; k < t->ntoks; k++) {
    Tok *tk = &t->toks[k];
    Ins in; in.op = 0; in.flags = 0; in.fframe = 0; in.fslot = 0; in.undef = 0;
    in.a = 0; in.b = 0; in.text = str_make(0, 0); in.text_esc = str_make(0, 0); in.segs = 0; in.nsegs = 0;
    if (tk->kind == TK_TEXT) {
      in.op = OP_TEXT; in.text = tk->text;
      /* JSON-escape once at compile time; the fused emitter copies it
       * raw instead of scanning the literal on every page render. */
      Out e; out_init(&e, tk->text.n * 2 + 64);
      out_adds_esc(&e, tk->text);
      in.text_esc = out_str(&e);
      cc_emit_ins(c, &in);
      continue;
    }
    if (tk->kind == TK_VAR || tk->kind == TK_RAW) {
      u8 fr, sl;
      int ok = tk->nsegs > 0 && cc_resolve(c, tk->segs[0], &fr, &sl);
      in.op = tk->kind == TK_VAR ? OP_VAR : OP_RAW;
      if (!ok) { in.undef = 1; }
      else { in.fframe = fr; in.fslot = sl; in.segs = tk->segs + 1; in.nsegs = tk->nsegs - 1; }
      cc_emit_ins(c, &in);
      continue;
    }
    /* A tag. */
    TagInfo inf; Str err = str_make(0, 0);
    if (!tag_parse(tk->text, t->name, &inf, &err)) {
      /* Not a tag: mirror V6's error text. */
      Buf b; buf_init(&b, 128);
      buf_addlit(&b, "template \""); buf_adds(&b, t->name);
      buf_addlit(&b, "\": {% ");
      buf_adds(&b, tk->text);
      buf_addlit(&b, " %} is not a tag");
      *err_out = buf_str(&b);
      return -1;
    }
    if (inf.kind == TG_FOR) {
      if (c->sp - sp0 >= 16 || c->nfor >= 31) {
        Buf b; buf_init(&b, 128);
        buf_addlit(&b, "template \""); buf_adds(&b, t->name);
        buf_addlit(&b, "\": {% ");
        buf_adds(&b, tk->text);
        buf_addlit(&b, " %} is not a tag");
        *err_out = buf_str(&b);
        return -1;
      }
      u8 fr, sl;
      int ok = inf.nsegs > 0 && cc_resolve(c, inf.segs[0], &fr, &sl);
      in.op = OP_FOR; in.b = inf.var;
      if (!ok) { in.undef = 1; }
      else { in.fframe = fr; in.fslot = sl; in.segs = inf.segs + 1; in.nsegs = inf.nsegs - 1; }
      u32 fi = c->nins;
      cc_emit_ins(c, &in);
      c->st[c->sp].kind = 0; c->st[c->sp].ins = fi; c->st[c->sp].has_else = 0; c->st[c->sp].jmp = 0; c->st[c->sp].name = t->name;
      c->sp++;
      c->nfor++;
      if (c->nfs + 2 <= 64) {
        c->fs_name[c->nfs] = inf.var; c->fs_frame[c->nfs] = (u8)c->nfor; c->fs_slot[c->nfs] = 0; c->nfs++;
        c->fs_name[c->nfs] = ID_loop; c->fs_frame[c->nfs] = (u8)c->nfor; c->fs_slot[c->nfs] = 1; c->nfs++;
      }
      continue;
    }
    if (inf.kind == TG_IF) {
      if (c->sp - sp0 >= 16) {
        Buf b; buf_init(&b, 128);
        buf_addlit(&b, "template \""); buf_adds(&b, t->name);
        buf_addlit(&b, "\": {% ");
        buf_adds(&b, tk->text);
        buf_addlit(&b, " %} is not a tag");
        *err_out = buf_str(&b);
        return -1;
      }
      u8 fr, sl;
      int ok = inf.nsegs > 0 && cc_resolve(c, inf.segs[0], &fr, &sl);
      in.op = OP_IF; in.flags = (u8)inf.not;
      if (!ok) { in.undef = 1; }
      else { in.fframe = fr; in.fslot = sl; in.segs = inf.segs + 1; in.nsegs = inf.nsegs - 1; }
      u32 fi = c->nins;
      cc_emit_ins(c, &in);
      c->st[c->sp].kind = 1; c->st[c->sp].ins = fi; c->st[c->sp].has_else = 0; c->st[c->sp].jmp = 0; c->st[c->sp].name = t->name;
      c->sp++;
      continue;
    }
    if (inf.kind == TG_ELSE) {
      if (c->sp <= sp0 || c->st[c->sp - 1].kind != 1 || c->st[c->sp - 1].has_else) {
        Buf b; buf_init(&b, 96);
        buf_addlit(&b, "template \""); buf_adds(&b, t->name);
        buf_addlit(&b, "\": an else outside an if");
        *err_out = buf_str(&b);
        return -1;
      }
      in.op = OP_JMP;
      c->st[c->sp - 1].jmp = c->nins;
      cc_emit_ins(c, &in);
      c->ins[c->st[c->sp - 1].ins].a = c->nins; /* else_pc */
      c->st[c->sp - 1].has_else = 1;
      continue;
    }
    if (inf.kind == TG_ENDFOR || inf.kind == TG_ENDIF) {
      int want_for = inf.kind == TG_ENDFOR;
      if (c->sp <= sp0 || c->st[c->sp - 1].kind != (want_for ? 0 : 1)) {
        Buf b; buf_init(&b, 96);
        buf_addlit(&b, "template \""); buf_adds(&b, t->name);
        buf_addlit(&b, "\": an ");
        buf_adds(&b, tk->text);
        buf_addlit(&b, " with no open ");
        buf_addlit(&b, want_for ? "for" : "if");
        *err_out = buf_str(&b);
        return -1;
      }
      u32 fi = c->st[c->sp - 1].ins;
      Str fname = c->st[c->sp - 1].name;
      (void)fname;
      c->sp--;
      if (want_for) {
        in.op = OP_NEXT; in.a = fi;
        cc_emit_ins(c, &in);
        c->ins[fi].a = c->nins; /* end_pc (past NEXT) */
        c->nfor--;
        if (c->nfs >= 2) c->nfs -= 2;
      } else {
        c->ins[fi].b = c->nins; /* end_pc */
        if (c->st[c->sp].has_else) c->ins[c->st[c->sp].jmp].a = c->nins;
        else c->ins[fi].a = c->nins;
      }
      continue;
    }
    if (inf.kind == TG_INCLUDE) {
      int idx = tpl_index_of(c->tpls, c->ntpls, inf.inc_name);
      if (idx < 0) continue; /* missing template renders nothing */
      Tpl *sub = &c->tpls[idx];
      if (sub->failed) continue;
      if (c->depth >= 8) continue; /* INCLUDE_DEPTH */
      if (sub->prerender) {
        in.op = OP_SPLICE; in.a = (u32)idx;
        cc_emit_ins(c, &in);
        continue;
      }
      int saved_nins = (int)c->nins, saved_sp = c->sp, saved_nfs = c->nfs, saved_nfor = c->nfor;
      int saved_depth = c->depth;
      c->depth = saved_depth + 1;
      int r = cc_emit_tpl(c, idx);
      c->depth = saved_depth;
      if (r < 0) {
        /* The callee is broken: it is marked failed (its error recorded
         * by cc_emit_tpl); rewind its partial emission and render nothing. */
        c->nins = (u32)saved_nins; c->sp = saved_sp; c->nfs = saved_nfs; c->nfor = saved_nfor;
      }
      continue;
    }
    /* TG_BAD handled above (tag_parse returned 0). */
    Buf b; buf_init(&b, 128);
    buf_addlit(&b, "template \""); buf_adds(&b, t->name);
    buf_addlit(&b, "\": {% ");
    buf_adds(&b, tk->text);
    buf_addlit(&b, " %} is not a tag");
    *err_out = buf_str(&b);
    return -1;
  }
  if (c->sp > sp0) {
    Buf b; buf_init(&b, 96);
    buf_addlit(&b, "template \"");
    buf_adds(&b, c->st[sp0].name);
    buf_addlit(&b, "\": a ");
    buf_addlit(&b, c->st[sp0].kind == 0 ? "for" : "if");
    buf_addlit(&b, " is not closed");
    *err_out = buf_str(&b);
    return -1;
  }
  return 1;
}

static int cc_emit_tpl(CCtx *c, int tidx) {
  Tpl *t = &c->tpls[tidx];
  if (t->failed) return 0;
  Str err = str_make(0, 0);
  int r = cc_emit_tokens(c, t, &err);
  if (r < 0) {
    t->failed = 1;
    t->err = err;
    return -1;
  }
  return 1;
}

/* Compile a template as a root program for context `ctx` (memoized).
 * Returns 1 on success (prog filled), 0 when the template failed. */
static int tpl_root_prog(Tpl *tpls, u32 ntpls, int tidx, int ctx) {
  Tpl *t = &tpls[tidx];
  if (t->prog_done[ctx]) return t->failed ? 0 : 1;
  t->prog_done[ctx] = 1;
  if (t->failed) return 0;
  CCtx c;
  c.tpls = tpls; c.ntpls = ntpls; c.ctx = ctx;
  c.ins = 0; c.nins = 0; c.cap = 0;
  c.nfs = 0; c.nfor = 0; c.depth = 0; c.sp = 0;
  int r = cc_emit_tpl(&c, tidx);
  if (r < 0) return 0; /* failed (err recorded on the template) */
  Ins end; end.op = OP_END; end.flags = 0; end.fframe = 0; end.fslot = 0; end.undef = 0;
  end.a = 0; end.b = 0; end.text = str_make(0, 0); end.segs = 0; end.nsegs = 0;
  cc_emit_ins(&c, &end);
  /* Copy the scratch instruction list into the retained arena. */
  Ins *prog = (Ins *)ralloc(sizeof(Ins) * c.nins);
  memcpy(prog, c.ins, sizeof(Ins) * c.nins);
  t->prog[ctx] = prog; t->nprog[ctx] = c.nins;
  return 1;
}

#endif
