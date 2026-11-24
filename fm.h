/* Front matter: makefun-site's strict subset (lib/frontmatter.js).
 * Parses a source's front matter and body, or the exact refusal reason.
 * Semantics identical to the proven V6 implementation; only the top lines
 * of the file are ever scanned (the body is never split into lines).
 */
#ifndef SG_FM_H
#define SG_FM_H

#include "rt.h"
#include "text.h"

typedef struct { Str s; u32 next; } Line;

static const char *FM_KEYS[] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};
#define FM_NKEYS 7

typedef struct {
  Str title, summary, date, layout, slug;
  Str *tags; u32 ntags;
  int draft;
  u8 has_title, has_summary, has_date, has_layout, has_slug, has_draft, has_tags;
} FMData;

typedef struct {
  int ok;
  Str error;
  FMData data;
  Str body;
} FMResult;

static int fm_days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

/* A string value: quoted (only \" and \\ escapes) or bare. */
static Str fm_string_of(Str raw, Str *err) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') { *err = str_c("a quoted string is not closed"); return str_make(0, 0); }
    RBuf b; rbuf_init(&b, raw.n);
    for (u32 i = 1; i + 1 < raw.n; i++) {
      char c = raw.p[i];
      if (c == '\\') {
        char nx = raw.p[i + 1];
        if ((nx != '"' && nx != '\\') || i + 1 == raw.n - 1) { *err = str_c("a quoted string holds an escape other than \\\" or \\\\"); return str_make(0, 0); }
        rbuf_addc(&b, nx);
        i++;
      } else if (c == '"') { *err = str_c("a quoted string holds a stray quote"); return str_make(0, 0); }
      else rbuf_addc(&b, c);
    }
    return str_make(b.p, b.n);
  }
  if (raw.n > 0 && raw.p[0] == '[') { *err = str_c("a bare string opens on ["); return str_make(0, 0); }
  return raw;
}

static Str fm_value_of(Str key, Str raw, FMData *d, Str *err) {
  if (str_eq(key, str_c("title")) || str_eq(key, str_c("summary"))) {
    Str e = str_make(0, 0);
    Str v = fm_string_of(raw, &e);
    if (e.p) {
      RBuf b; rbuf_init(&b, 64);
      rbuf_addlit(&b, "key \"");
      rbuf_add(&b, key.p, key.n);
      rbuf_addlit(&b, "\": ");
      rbuf_add(&b, e.p, e.n);
      *err = str_make(b.p, b.n);
      return str_make(0, 0);
    }
    return v;
  }
  if (str_eq(key, str_c("date"))) {
    int ok = raw.n == 10 && is_digit(raw.p[0]) && is_digit(raw.p[1]) && is_digit(raw.p[2]) && is_digit(raw.p[3]) &&
             raw.p[4] == '-' && is_digit(raw.p[5]) && is_digit(raw.p[6]) && raw.p[7] == '-' &&
             is_digit(raw.p[8]) && is_digit(raw.p[9]);
    int y = 0, mo = 0, dd = 0;
    if (ok) {
      y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
      mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
      dd = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
      if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || dd < 1 || dd > fm_days_in(y, mo)) ok = 0;
    }
    if (!ok) { *err = str_c("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)"); return str_make(0, 0); }
    return raw;
  }
  if (str_eq(key, str_c("tags"))) {
    if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') { *err = str_c("key \"tags\": not a list [a, b]"); return str_make(0, 0); }
    Str inner = str_make(raw.p + 1, raw.n - 2);
    Str t = js_trim(inner);
    if (t.n == 0) { d->tags = 0; d->ntags = 0; d->has_tags = 1; return raw; }
    u32 cnt = 1;
    for (u32 i = 0; i < inner.n; i++) if (inner.p[i] == ',') cnt++;
    Str *tags = (Str *)ralloc(sizeof(Str) * cnt);
    u32 n = 0, pos = 0;
    for (u32 i = 0; i <= inner.n; i++) {
      if (i == inner.n || inner.p[i] == ',') {
        Str one = js_trim(str_make(inner.p + pos, i - pos));
        if (one.n == 0) { *err = str_c("key \"tags\": an empty tag, or one holding \" [ or ]"); return str_make(0, 0); }
        for (u32 k = 0; k < one.n; k++) {
          char c = one.p[k];
          if (c == '"' || c == '[' || c == ']') { *err = str_c("key \"tags\": an empty tag, or one holding \" [ or ]"); return str_make(0, 0); }
        }
        tags[n++] = one;
        pos = i + 1;
      }
    }
    d->tags = tags; d->ntags = n; d->has_tags = 1;
    return raw;
  }
  if (str_eq(key, str_c("draft"))) {
    if (str_eq(raw, str_c("true"))) { d->draft = 1; return raw; }
    if (str_eq(raw, str_c("false"))) { d->draft = 0; return raw; }
    *err = str_c("key \"draft\": true or false");
    return str_make(0, 0);
  }
  if (str_eq(key, str_c("layout"))) {
    int ok = raw.n > 0;
    for (u32 i = 0; i < raw.n; i++) { char c = raw.p[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) { ok = 0; break; } }
    if (!ok) { *err = str_c("key \"layout\": not a template name"); return str_make(0, 0); }
    return raw;
  }
  if (str_eq(key, str_c("slug"))) {
    int ok = raw.n > 0 && raw.n <= 64;
    if (ok) {
      int seg = 0;
      for (u32 i = 0; i < raw.n; i++) {
        char c = raw.p[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) seg = 1;
        else if (c == '-' && seg) seg = 0;
        else { ok = 0; break; }
      }
      if (seg == 0) ok = 0;
    }
    if (!ok) { *err = str_c("key \"slug\": not a slug"); return str_make(0, 0); }
    return raw;
  }
  *err = str_make(0, 0);
  return raw;
}

static FMResult parse_front_matter(Str text) {
  FMResult r; r.ok = 0; r.error = str_make(0, 0); r.body = str_make(0, 0);
  FMData *d = &r.data;
  d->title = d->summary = d->date = d->layout = d->slug = str_make(0, 0);
  d->tags = 0; d->ntags = 0; d->draft = 0;
  d->has_title = d->has_summary = d->has_date = d->has_layout = d->has_slug = d->has_draft = d->has_tags = 0;

  if (text.n < 3 || memcmp(text.p, "---", 3) != 0 || (text.n > 3 && text.p[3] != '\n')) { r.error = str_c("no front matter"); return r; }
  /* Scan the front-matter lines only (scratch array; lines point into text). */
  u32 cap = 16, nlines = 0;
  Line *lines = (Line *)salloc(sizeof(Line) * cap);
  lines[nlines].s = str_make(text.p, 3);
  lines[nlines].next = 4;
  nlines++;
  int close = -1;
  u32 pos = 4;
  for (;;) {
    if (pos >= text.n) break;
    u32 start = pos;
    while (pos < text.n && text.p[pos] != '\n') pos++;
    u32 ln = pos - start;
    u32 next = (pos < text.n) ? pos + 1 : (u32)-1;
    if (nlines == cap) {
      cap *= 2;
      Line *nl2 = (Line *)salloc(sizeof(Line) * cap);
      memcpy(nl2, lines, sizeof(Line) * nlines);
      lines = nl2;
    }
    lines[nlines].s = str_make(text.p + start, ln);
    lines[nlines].next = next;
    if (str_eq(lines[nlines].s, str_c("---"))) { close = (int)nlines; nlines++; break; }
    nlines++;
    if (next == (u32)-1) break;
    pos = next;
  }
  if (close < 0) { r.error = str_c("the front matter is not closed"); return r; }

  u8 seen[FM_NKEYS]; for (int i = 0; i < FM_NKEYS; i++) seen[i] = 0;
  for (int i = 1; i < close; i++) {
    Str line = lines[i].s;
    if (is_blank_line(line.p, line.n)) continue;
    if (line.n > 0 && line.p[0] == '#') continue;
    u32 k = 0;
    while (k < line.n && (is_lower(line.p[k]) || line.p[k] == '_')) k++;
    if (k == 0 || k >= line.n || line.p[k] != ':') {
      RBuf b; rbuf_init(&b, 48);
      rbuf_addlit(&b, "line ");
      char tmp[12]; int tn = 0; int v = i + 1;
      char rev[12]; int rn = 0; do { rev[rn++] = (char)('0' + v % 10); v /= 10; } while (v);
      while (rn) tmp[tn++] = rev[--rn];
      rbuf_add(&b, tmp, (u32)tn);
      rbuf_addlit(&b, " is not \"key: value\"");
      r.error = str_make(b.p, b.n);
      return r;
    }
    Str key = str_make(line.p, k);
    Str raw;
    if (k + 1 == line.n) raw = str_make(line.p + k + 1, 0);
    else if (line.p[k + 1] == ' ') raw = str_make(line.p + k + 2, line.n - (k + 2));
    else {
      RBuf b; rbuf_init(&b, 48);
      rbuf_addlit(&b, "line ");
      char tmp[12]; int tn = 0; int v = i + 1;
      char rev[12]; int rn = 0; do { rev[rn++] = (char)('0' + v % 10); v /= 10; } while (v);
      while (rn) tmp[tn++] = rev[--rn];
      rbuf_add(&b, tmp, (u32)tn);
      rbuf_addlit(&b, " is not \"key: value\"");
      r.error = str_make(b.p, b.n);
      return r;
    }
    while (raw.n > 0 && is_space_tab(raw.p[raw.n - 1])) raw.n--;
    int ki = -1;
    for (int q = 0; q < FM_NKEYS; q++) if (str_eq(key, str_c(FM_KEYS[q]))) { ki = q; break; }
    if (ki < 0) {
      RBuf b; rbuf_init(&b, 64);
      rbuf_addlit(&b, "unknown key \"");
      rbuf_add(&b, key.p, key.n);
      rbuf_addc(&b, '"');
      r.error = str_make(b.p, b.n);
      return r;
    }
    if (seen[ki]) {
      RBuf b; rbuf_init(&b, 64);
      rbuf_addlit(&b, "key \"");
      rbuf_add(&b, key.p, key.n);
      rbuf_addlit(&b, "\" is given twice");
      r.error = str_make(b.p, b.n);
      return r;
    }
    seen[ki] = 1;
    if (raw.n == 0) {
      RBuf b; rbuf_init(&b, 64);
      rbuf_addlit(&b, "key \"");
      rbuf_add(&b, key.p, key.n);
      rbuf_addlit(&b, "\" has no value");
      r.error = str_make(b.p, b.n);
      return r;
    }
    Str err = str_make(0, 0);
    Str val = fm_value_of(key, raw, d, &err);
    if (err.p) { r.error = err; return r; }
    switch (ki) {
      case 0: d->title = val; d->has_title = 1; break;
      case 1: d->date = val; d->has_date = 1; break;
      case 2: d->has_tags = 1; break;
      case 3: d->has_draft = 1; break;
      case 4: d->layout = val; d->has_layout = 1; break;
      case 5: d->slug = val; d->has_slug = 1; break;
      case 6: d->summary = val; d->has_summary = 1; break;
    }
  }
  if (!d->has_title) { r.error = str_c("no title"); return r; }
  if (lines[close].next == (u32)-1) r.body = str_make(text.p + text.n, 0);
  else r.body = str_make(text.p + lines[close].next, text.n - lines[close].next);
  r.ok = 1;
  return r;
}

#endif
