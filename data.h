/* Shared site data types, and template object field access.
 * The object shapes follow makefun-site's index.js exactly.
 */
#ifndef SG_DATA_H
#define SG_DATA_H

#include "rt.h"
#include "text.h"
#include "tpl.h"
#include "md.h"

typedef struct Post Post;
typedef struct Tag Tag;

typedef struct {
  Str name, slug, url;
  Tag *tag;
} TagRef;

struct Post {
  Str path;
  Str layout;
  Str fm_slug; int has_fm_slug;
  Str *fm_tags; u32 nfm_tags; int has_fm_tags;
  Str body;
  Str slug;
  Str out_dir;
  Str url;
  Str title;
  Str date;
  Str date_long;
  Str summary;
  Str excerpt;
  long reading_minutes;
  TagRef *tags; u32 ntags;
  Str content;
  TocEnt *toc; u32 ntoc;
  Post *newer; Post *older;
  int is_post;
};

struct Tag {
  Str name, slug, url;
  Post **posts; u32 nposts;
};

typedef struct {
  Str title, base_url;
} SiteInfo;

typedef struct {
  long number, total;
  Str url, newer_url, older_url;
} PagerVal;

typedef struct {
  long index, length;
  int first, last;
} LoopVal;

typedef struct {
  Str name, slug, url;
  long count;
} TagSum;

/* ---- values ---- */
typedef enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_ARR, V_OBJ } VKind;
typedef enum { O_POST, O_TAG, O_SITE, O_PAGER, O_LOOP, O_TOC, O_CARD, O_LINK, O_TAGSUM, O_TAGREF } OType;

typedef struct Val Val;
struct Val {
  u8 kind;
  union {
    int b;
    long num;
    Str s;
    struct { Val *v; u32 n; } arr;
    struct { u8 otype; void *p; } obj;
  };
};

static Val v_undef(void) { Val v; v.kind = V_UNDEF; v.num = 0; return v; }
static Val v_null(void) { Val v; v.kind = V_NULL; v.num = 0; return v; }
static Val v_bool(int b) { Val v; v.kind = V_BOOL; v.b = b; return v; }
static Val v_num(long n) { Val v; v.kind = V_NUM; v.num = n; return v; }
static Val v_str(Str s) { Val v; v.kind = V_STR; v.s = s; return v; }
static Val v_obj(u8 otype, void *p) { Val v; v.kind = V_OBJ; v.obj.otype = otype; v.obj.p = p; return v; }
static Val v_arr(Val *items, u32 n) { Val v; v.kind = V_ARR; v.arr.v = items; v.arr.n = n; return v; }

static void val_text_out(Out *o, Val v) {
  switch (v.kind) {
    case V_STR: out_adds(o, v.s); return;
    case V_NUM: {
      char tmp[24]; int n = 0; long x = v.num;
      if (x == 0) { out_c(o, '0'); return; }
      if (x < 0) { out_c(o, '-'); x = -x; }
      char rev[24]; int rn = 0;
      while (x) { rev[rn++] = (char)('0' + x % 10); x /= 10; }
      while (rn) tmp[n++] = rev[--rn];
      out_add(o, tmp, (u32)n);
      return;
    }
    case V_BOOL: out_lit(o, v.b ? "true" : "false"); return;
    case V_ARR:
      for (u32 i = 0; i < v.arr.n; i++) {
        if (i) out_lit(o, ", ");
        val_text_out(o, v.arr.v[i]);
      }
      return;
    default: return;
  }
}

static int val_truthy(Val v) {
  if (v.kind == V_ARR) return v.arr.n > 0;
  if (v.kind == V_UNDEF || v.kind == V_NULL) return 0;
  if (v.kind == V_BOOL) return v.b;
  if (v.kind == V_STR) return v.s.n > 0;
  if (v.kind == V_NUM) return v.num != 0;
  return 1;
}

static Val cards_of(Post **posts, u32 n) {
  Val *items = (Val *)ralloc(sizeof(Val) * (n ? n : 1));
  for (u32 i = 0; i < n; i++) items[i] = v_obj(O_CARD, posts[i]);
  return v_arr(items, n);
}

static Val obj_field(Val obj, u32 field) {
  if (obj.kind != V_OBJ) return v_undef();
  switch (obj.obj.otype) {
    case O_CARD: {
      Post *p = (Post *)obj.obj.p;
      if (field == ID_title) return v_str(p->title);
      if (field == ID_url) return v_str(p->url);
      if (field == ID_date) return v_str(p->date);
      if (field == ID_date_long) return v_str(p->date_long);
      if (field == ID_summary) return v_str(p->summary);
      if (field == ID_excerpt) return v_str(p->excerpt);
      if (field == ID_reading_minutes) return v_num(p->reading_minutes);
      if (field == ID_tags) {
        Val *items = (Val *)ralloc(sizeof(Val) * (p->ntags ? p->ntags : 1));
        for (u32 i = 0; i < p->ntags; i++) items[i] = v_obj(O_TAGREF, &p->tags[i]);
        return v_arr(items, p->ntags);
      }
      return v_undef();
    }
    case O_POST: {
      Post *p = (Post *)obj.obj.p;
      if (field == ID_title) return v_str(p->title);
      if (field == ID_url) return v_str(p->url);
      if (field == ID_date) return v_str(p->date);
      if (field == ID_date_long) return v_str(p->date_long);
      if (field == ID_summary) return v_str(p->summary);
      if (field == ID_excerpt) return v_str(p->excerpt);
      if (field == ID_reading_minutes) return v_num(p->reading_minutes);
      if (field == ID_slug) return v_str(p->slug);
      if (field == ID_content) return v_str(p->content);
      if (field == ID_toc) {
        Val *items = (Val *)ralloc(sizeof(Val) * (p->ntoc ? p->ntoc : 1));
        for (u32 i = 0; i < p->ntoc; i++) items[i] = v_obj(O_TOC, &p->toc[i]);
        return v_arr(items, p->ntoc);
      }
      if (field == ID_tags) {
        Val *items = (Val *)ralloc(sizeof(Val) * (p->ntags ? p->ntags : 1));
        for (u32 i = 0; i < p->ntags; i++) items[i] = v_obj(O_TAGREF, &p->tags[i]);
        return v_arr(items, p->ntags);
      }
      if (field == ID_newer) return p->newer ? v_obj(O_LINK, p->newer) : v_null();
      if (field == ID_older) return p->older ? v_obj(O_LINK, p->older) : v_null();
      if (field == ID_is_post) return v_bool(p->is_post);
      return v_undef();
    }
    case O_LINK: {
      Post *p = (Post *)obj.obj.p;
      if (field == ID_title) return v_str(p->title);
      if (field == ID_url) return v_str(p->url);
      if (field == ID_date) return v_str(p->date);
      if (field == ID_date_long) return v_str(p->date_long);
      return v_undef();
    }
    case O_TAG: {
      Tag *t = (Tag *)obj.obj.p;
      if (field == ID_name) return v_str(t->name);
      if (field == ID_url) return v_str(t->url);
      if (field == ID_slug) return v_str(t->slug);
      if (field == ID_count) return v_num((long)t->nposts);
      if (field == ID_posts) return cards_of(t->posts, t->nposts);
      return v_undef();
    }
    case O_TAGSUM: {
      TagSum *t = (TagSum *)obj.obj.p;
      if (field == ID_name) return v_str(t->name);
      if (field == ID_slug) return v_str(t->slug);
      if (field == ID_url) return v_str(t->url);
      if (field == ID_count) return v_num(t->count);
      return v_undef();
    }
    case O_TAGREF: {
      TagRef *r = (TagRef *)obj.obj.p;
      if (field == ID_name) return v_str(r->name);
      if (field == ID_slug) return v_str(r->slug);
      if (field == ID_url) return v_str(r->url);
      if (field == ID_posts) {
        if (!r->tag) return v_arr((Val *)ralloc(sizeof(Val)), 0);
        return cards_of(r->tag->posts, r->tag->nposts);
      }
      return v_undef();
    }
    case O_SITE: {
      SiteInfo *s = (SiteInfo *)obj.obj.p;
      if (field == ID_title) return v_str(s->title);
      if (field == ID_base_url) return v_str(s->base_url);
      return v_undef();
    }
    case O_PAGER: {
      PagerVal *pg = (PagerVal *)obj.obj.p;
      if (field == ID_number) return v_num(pg->number);
      if (field == ID_total) return v_num(pg->total);
      if (field == ID_url) return v_str(pg->url);
      if (field == ID_newer_url) return v_str(pg->newer_url);
      if (field == ID_older_url) return v_str(pg->older_url);
      return v_undef();
    }
    case O_LOOP: {
      LoopVal *l = (LoopVal *)obj.obj.p;
      if (field == ID_index) return v_num(l->index);
      if (field == ID_first) return v_bool(l->first);
      if (field == ID_last) return v_bool(l->last);
      if (field == ID_length) return v_num(l->length);
      return v_undef();
    }
    case O_TOC: {
      TocEnt *e = (TocEnt *)obj.obj.p;
      if (field == ID_level) return v_num(e->level);
      if (field == ID_id) return v_str(e->id);
      if (field == ID_html) return v_str(e->html);
      return v_undef();
    }
  }
  return v_undef();
}

/* textOf(v), HTML-escaped, straight to the output. */
static void val_escape_out(Out *o, Val v) {
  switch (v.kind) {
    case V_STR: escape_html_out(o, v.s); return;
    case V_NUM: case V_BOOL: val_text_out(o, v); return;
    case V_ARR:
      for (u32 i = 0; i < v.arr.n; i++) {
        if (i) out_lit(o, ", ");
        val_escape_out(o, v.arr.v[i]);
      }
      return;
    default: return;
  }
}

/* Fused emit: textOf(v), HTML-escaped AND JSON-escaped in sequence
 * (HTML into a reusable scratch buffer, then JSON-escape-copy to the
 * answers cursor). Byte-identical to val_escape_out + json_escape_out,
 * but the page never sits in an intermediate buffer. NUM/BOOL text is
 * JSON-clean, so it goes straight out. */
/* C1 (ronde 7/10): VAR fast path -- if the value needs neither HTML nor
 * JSON escaping, copy it straight out (saves one HTML-escape scan + one
 * tmp round-trip + one JSON-escape scan). */
static int str_var_clean(Str s) {
  const char *p = s.p, *end = s.p + s.n;
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m = _mm_cmpeq_epi8(v, JE_Q);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, JE_BS));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[0])); /* & */
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[1])); /* < */
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[2])); /* > */
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ESC_V[4])); /* ' */
    __m128i vb = _mm_xor_si128(v, JE_BIAS);
    m = _mm_or_si128(m, _mm_cmplt_epi8(vb, JE_LIM)); /* < 0x20 */
    if (_mm_movemask_epi8(m)) return 0;
    p += 16;
  }
  while (p < end) {
    u8 c = (u8)*p;
    if (c < 0x20 || c == '"' || c == '\\' || c == '&' || c == '<' || c == '>' || c == '\'') return 0;
    p++;
  }
  return 1;
}
/* V42 (ronde 10): force-inline into render_prog's OP_VAR -- 34k calls
 * and objdump showed it as a real call. The V_ARR case lives in a
 * separate helper so the inlined function is not directly recursive
 * (gcc rejects always_inline on self-recursive functions). */
static void val_escape_json_arr(Out *o, Val v, Buf *tmp);
static inline __attribute__((always_inline)) void val_escape_json_out(Out *o, Val v, Buf *tmp) {
  switch (v.kind) {
    case V_STR:
      if (str_var_clean(v.s)) { out_adds(o, v.s); return; }
      tmp->n = 0;
      escape_html_into(tmp, v.s);
      out_adds_esc(o, buf_str(tmp));
      return;
    case V_NUM: case V_BOOL: val_text_out(o, v); return;
    case V_ARR: val_escape_json_arr(o, v, tmp); return;
    default: return;
  }
}
static void val_escape_json_arr(Out *o, Val v, Buf *tmp) {
  for (u32 i = 0; i < v.arr.n; i++) {
    if (i) out_lit(o, ", ");
    val_escape_json_out(o, v.arr.v[i], tmp);
  }
}

/* Fused emit: RAW value (already HTML), JSON-escaped straight out. */
/* V42 (ronde 10): force-inline into render_prog's OP_RAW (see above). */
static void val_text_json_arr(Out *o, Val v);
static inline __attribute__((always_inline)) void val_text_json_out(Out *o, Val v) {
  switch (v.kind) {
    case V_STR: out_adds_esc(o, v.s); return;
    case V_NUM: case V_BOOL: val_text_out(o, v); return;
    case V_ARR: val_text_json_arr(o, v); return;
    default: return;
  }
}
static void val_text_json_arr(Out *o, Val v) {
  for (u32 i = 0; i < v.arr.n; i++) {
    if (i) out_lit(o, ", ");
    val_text_json_out(o, v.arr.v[i]);
  }
}

#endif
