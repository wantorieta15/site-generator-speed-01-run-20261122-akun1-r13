/* Driver: read cases.json from MAKEFUN_INPUT_DIR (default /input) or
 * argv[1], build each case with the fused pipeline in site.h, and write
 * answers.json to MAKEFUN_OUTPUT_DIR (default /output) or argv[2].
 * Freestanding: raw syscalls only.
 */
#include "rt.h"
#include "json.h"
#include "site.h"

static int write_all(int fd, const char *p, u32 n) {
  while (n > 0) {
    long r = sys3(SYS_WRITE, fd, (long)p, (long)n);
    if (r <= 0) return -1;
    p += r; n -= (u32)r;
  }
  return 0;
}

static int read_all(int fd, char **out, u32 *n) {
  /* Size the buffer from fstat so a 2.9 MB input never doubles/re-copies. */
  char st[144];
  u32 cap = 1u << 20, len = 0;
  if (sys3(SYS_FSTAT, fd, (long)st, 0) == 0) {
    long sz;
    memcpy(&sz, st + 48, 8); /* st_size at offset 48 (x86-64 struct stat) */
    if (sz > 0 && sz < (1L << 31)) {
      cap = (u32)((sz + 4095) & ~4095L);
      if (cap < (1u << 20)) cap = 1u << 20;
    }
  }
  char *buf = (char *)ralloc(cap);
  for (;;) {
    if (len == cap) {
      u32 nc = cap * 2;
      char *nb = (char *)ralloc(nc);
      memcpy(nb, buf, len);
      buf = nb; cap = nc;
    }
    long r = sys3(SYS_READ, fd, (long)(buf + len), (long)(cap - len));
    if (r < 0) return -1;
    if (r == 0) break;
    len += (u32)r;
  }
  *out = buf; *n = len;
  return 0;
}

static const char *env_get(char **envp, const char *name) {
  u32 nl = (u32)strlen(name);
  for (int i = 0; envp[i]; i++) {
    if (memcmp(envp[i], name, nl) == 0 && envp[i][nl] == '=') return envp[i] + nl + 1;
  }
  return 0;
}

static void join_path(char *out, const char *dir, const char *file) {
  u32 n = 0;
  while (dir[n]) { out[n] = dir[n]; n++; }
  if (n == 0 || out[n - 1] != '/') out[n++] = '/';
  u32 m = 0;
  while (file[m]) out[n++] = file[m++];
  out[n] = 0;
}

typedef struct { Out *ans; int first; u32 ncases; } MainCtx;

static void on_case(Input *in, void *ctx) {
  MainCtx *m = (MainCtx *)ctx;
  if (!m->first) out_c(m->ans, ',');
  m->first = 0;
  m->ncases++;
  Build bd;
  memset(&bd, 0, sizeof(bd));
  build_case(&bd, in, m->ans);
}

int main(int argc, char **argv, char **envp) {
  if (arena_init() != 0) return 1;
  tpl_intern_names();
  text_init();
  md_init();
  json_init();
  json_emit_init();

  const char *in_path = 0, *out_path = 0;
  if (argc >= 2) in_path = argv[1];
  if (argc >= 3) out_path = argv[2];
  char in_buf[4096], out_buf[4096];
  if (!in_path) {
    const char *in_dir = env_get(envp, "MAKEFUN_INPUT_DIR");
    if (!in_dir) in_dir = "/input";
    join_path(in_buf, in_dir, "cases.json");
    in_path = in_buf;
  }
  if (!out_path) {
    const char *out_dir = env_get(envp, "MAKEFUN_OUTPUT_DIR");
    if (!out_dir) out_dir = "/output";
    join_path(out_buf, out_dir, "answers.json");
    out_path = out_buf;
  }

  long fd = sys3(SYS_OPENAT, AT_FDCWD, (long)in_path, O_RDONLY);
  if (fd < 0) { write_stderr("cannot open cases.json\n", 22); return 1; }
  char *data; u32 ndata;
  if (read_all((int)fd, &data, &ndata) != 0) { write_stderr("cannot read cases.json\n", 22); return 1; }
  sys1(SYS_CLOSE, fd);

  Out answers;
  out_init(&answers, 1u << 24); /* ~7.4MB practice: no doubling re-copy */
  out_c(&answers, '[');
  MainCtx m; m.ans = &answers; m.first = 1; m.ncases = 0;
  if (!parse_cases(data, ndata, on_case, &m)) { write_stderr("bad cases.json\n", 14); return 1; }
  out_c(&answers, ']');

  long ofd = sys4(SYS_OPENAT, AT_FDCWD, (long)out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (ofd < 0) { write_stderr("cannot open answers.json\n", 24); return 1; }
  if (write_all((int)ofd, answers.p, (u32)(answers.cur - answers.p)) != 0) { write_stderr("cannot write answers.json\n", 25); return 1; }
  sys1(SYS_CLOSE, ofd);
  return 0;
}
