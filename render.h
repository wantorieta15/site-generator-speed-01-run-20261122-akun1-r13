/* The fused template interpreter: one flat loop over the compiled
 * instruction list. No recursion, no per-include dispatch, no name
 * lookups at render time (all scope slots were resolved at compile).
 *
 * Frames: frame 0 is the root frame (5 slots: site, tags, recent,
 * page/posts/tag, pager). Each enclosing for-loop owns a 2-slot frame
 * [loopvar, loop] at frame index = nesting depth. The interpreter tracks
 * the nesting depth in nfor; OP_FOR writes frame nfor+1, OP_NEXT reads
 * frame nfor.
 */
#ifndef SG_RENDER_H
#define SG_RENDER_H

#include "data.h"

typedef struct {
  Val arr; u32 idx, len, for_pc, end_pc;
} LoopSt;

typedef struct {
  Tpl *tpls; u32 ntpls;
  Out *out;
  int out_esc; /* render straight into the answers cursor, JSON-escaped */
  Buf var_tmp; /* reusable scratch for fused VAR escaping */
  Val root[5];
  Val *fr[40];       /* frame pointers by frame index */
  Val fbuf[40][2];   /* for-frame storage */
  LoopVal lbuf[40];  /* loop objects, reused per level */
  LoopSt ls[40];     /* loop state, indexed by frame */
  int nfor;
} RCtx;

static Val rc_walk(RCtx *rc, Ins *in) {
  Val v = rc->fr[in->fframe][in->fslot];
  for (u32 i = 0; i < in->nsegs; i++) v = obj_field(v, in->segs[i]);
  return v;
}

static void render_prog(RCtx *rc, Ins *prog, u32 nprog) {
  Out *o = rc->out;
  u32 pc = 0;
  while (pc < nprog) {
    Ins *in = &prog[pc];
    switch (in->op) {
      case OP_TEXT:
        if (rc->out_esc) out_adds(o, in->text_esc);
        else out_adds(o, in->text);
        pc++;
        break;
      case OP_VAR: {
        Val v = in->undef ? v_undef() : rc_walk(rc, in);
        if (rc->out_esc) val_escape_json_out(o, v, &rc->var_tmp);
        else val_escape_out(o, v);
        pc++;
        break;
      }
      case OP_RAW: {
        Val v = in->undef ? v_undef() : rc_walk(rc, in);
        if (rc->out_esc) val_text_json_out(o, v);
        else val_text_out(o, v);
        pc++;
        break;
      }
      case OP_IF: {
        Val v = in->undef ? v_undef() : rc_walk(rc, in);
        if (val_truthy(v) ^ in->flags) pc++;
        else pc = in->a; /* else_pc, or end_pc when there is no else */
        break;
      }
      case OP_JMP:
        pc = in->a;
        break;
      case OP_FOR: {
        Val v = in->undef ? v_undef() : rc_walk(rc, in);
        if (v.kind != V_ARR || v.arr.n == 0) { pc = in->a; break; }
        int fr = rc->nfor + 1;
        LoopSt *st = &rc->ls[fr];
        st->arr = v; st->idx = 0; st->len = v.arr.n; st->for_pc = pc; st->end_pc = in->a;
        LoopVal *lv = &rc->lbuf[fr];
        lv->index = 1; lv->length = (long)v.arr.n; lv->first = 1; lv->last = v.arr.n == 1; /* loop.index is 1-based */
        rc->fbuf[fr][0] = v.arr.v[0];
        rc->fbuf[fr][1] = v_obj(O_LOOP, lv);
        rc->nfor = fr;
        pc++;
        break;
      }
      case OP_NEXT: {
        int fr = rc->nfor;
        LoopSt *st = &rc->ls[fr];
        st->idx++;
        if (st->idx >= st->len) { rc->nfor = fr - 1; pc = st->end_pc; break; }
        LoopVal *lv = &rc->lbuf[fr];
        lv->index = (long)st->idx + 1; /* loop.index is 1-based */
        lv->first = st->idx == 0;
        lv->last = st->idx == st->len - 1;
        rc->fbuf[fr][0] = st->arr.arr.v[st->idx];
        pc = st->for_pc + 1;
        break;
      }
      case OP_SPLICE:
        if (rc->out_esc) out_adds(o, rc->tpls[in->a].pre_esc);
        else out_adds(o, rc->tpls[in->a].pre);
        pc++;
        break;
      default: /* OP_END */
        return;
    }
  }
}

/* Render a root program for template `tidx` in context `ctx` with the
 * given 5-slot root frame. Failed templates render nothing. */
static void render_tpl(RCtx *rc, int tidx, int ctx, Val *root5) {
  Tpl *t = &rc->tpls[tidx];
  if (!tpl_root_prog(rc->tpls, rc->ntpls, tidx, ctx)) return;
  memcpy(rc->root, root5, sizeof(Val) * 5);
  rc->fr[0] = rc->root;
  for (int i = 1; i < 40; i++) rc->fr[i] = rc->fbuf[i];
  rc->nfor = 0;
  render_prog(rc, t->prog[ctx], t->nprog[ctx]);
}

/* Pre-render a constant template (freevars subset of {site,tags,recent})
 * into a retained string. */
static Str render_pre_str(RCtx *rc, int tidx, Val site, Val tags, Val recent) {
  Tpl *t = &rc->tpls[tidx];
  if (!tpl_root_prog(rc->tpls, rc->ntpls, tidx, CTX_PAGE)) return str_make(0, 0);
  rc->root[0] = site; rc->root[1] = tags; rc->root[2] = recent;
  rc->root[3] = v_undef(); rc->root[4] = v_undef();
  rc->fr[0] = rc->root;
  for (int i = 1; i < 40; i++) rc->fr[i] = rc->fbuf[i];
  rc->nfor = 0;
  Out o; out_init(&o, 8192);
  Out *saved = rc->out;
  int saved_esc = rc->out_esc;
  rc->out = &o;
  rc->out_esc = 0;
  render_prog(rc, t->prog[CTX_PAGE], t->nprog[CTX_PAGE]);
  rc->out = saved;
  rc->out_esc = saved_esc;
  Str r = out_str(&o);
  /* JSON-escaped copy, for fused emit (plain copy at splice time). */
  Out e; out_init(&e, r.n * 2 + 64);
  out_adds_esc(&e, r);
  t->pre_esc = out_str(&e);
  return r;
}

#endif
