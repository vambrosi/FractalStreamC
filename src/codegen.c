/* codegen.c - checked AST of a viewer -> SPIR-V fragment shader.
 *
 * Shader interface: see agents/architecture.md.  The state buffer is a
 * BufferBlock in the Uniform storage class (the SPIR-V 1.0 way to declare a
 * storage buffer).  At entry, the globals the viewer uses are copied into
 * Private variables (so viewers may modify their own copy), the pixel is
 * mapped to the plane, `color` starts black, and at every exit the colour is
 * clamped and written to the output.
 *
 * Every function the viewer calls (user or prelude) becomes a SPIR-V
 * function; all locals are Function variables initialised to zero, exactly
 * like the interpreter's zeroed frames.  Loops follow the same iteration
 * limit / .stuck rules as interp.c. */
#include "ast.h"

typedef struct Gen {
    Spv s;
    Program *prog;
    Section *viewer;
    u32 tvoid, tbool, tint, tuint, tfloat, tvec2, tvec4;
    /* deep zoom: reals and complex numbers in double-single precision (see "df" below) */
    bool df;
    u32 treal, tcplx; /* the types of R and C: float and vec2, or (df) vec2 and vec4 */
    u32 state_var, pc_var, fragcoord, out_color;
    u32 stuck_var, iter_var, pxsize_var; /* .stuck, .iterations, .pixel */
    /* compute variant (flood fill): see codegen_viewer_flood */
    bool compute;
    u32 flood_mode_var, flood_seed_var, flood_from_var, flood_out_t_var, flood_out_ok_var;
    u32 field_var, colors_var, pixel_var; /* storage buffers and this invocation's pixel index */
    u32 flood_nb, flood_W;                /* the neighbour to continue from (pixel index), image width */
    u32 flood_pass_var;                   /* the pass's mode (FLOOD_PASS_*), for the output */
    Func *fn;
    struct { u32 merge, cont; } loops[64];
    int nloops;
    bool dead;
    VEC(Func *) queue;
    VEC(Func *) visited;
    char err[256];
} Gen;

static u32 gen_expr(Gen *g, Expr *e);
static void gen_block(Gen *g, Block *b);
static void return_if(Gen *g, u32 bad);
static u32 real_const(Gen *g, double v);
static void mark_expr(Gen *g, Expr *e);
static u32 proj_scale(Gen *g, u32 v);

/* ---- types ---------------------------------------------------------------- */
static u32 ty(Gen *g, Type *t) {
    if (t->spv_id) return t->spv_id;
    u32 id = 0;
    switch (t->kind) {
    case T_BOOL: id = g->tbool; break;
    case T_INT: id = g->tint; break;
    case T_REAL: id = g->treal; break;
    case T_COMPLEX: id = g->tcplx; break;
    case T_QUAT: id = g->tvec4; break; /* quaternions stay in single precision */
    case T_PROJ: id = g->tvec4; break; /* so do points of the sphere: (a, b) */
    case T_VEC: id = spv_type_array(&g->s, ty(g, t->elem), t->n); break;
    case T_LIST: {
        u32 m[2] = {g->tint, ty(g, type_vec(t->elem, t->n))};
        id = spv_type_struct(&g->s, m, 2);
        break;
    }
    case T_TUPLE: {
        u32 m[64];
        for (int i = 0; i < t->n && i < 64; i++) m[i] = ty(g, t->items[i]);
        id = spv_type_struct(&g->s, m, FS_MIN(t->n, 64));
        break;
    }
    default: id = g->tvoid; break;
    }
    t->spv_id = id;
    return id;
}

/* explicitly laid out type for the state buffer */
static u32 bty(Gen *g, Type *t) {
    if (t->spv_buf_id) return t->spv_buf_id;
    u32 id;
    switch (t->kind) {
    case T_BOOL: id = g->tuint; break;
    case T_REAL: id = g->tfloat; break; /* the state is in single precision, also for deep zooms */
    case T_COMPLEX: id = g->tvec2; break;
    case T_VEC:
        id = spv_type_array(&g->s, bty(g, t->elem), t->n);
        spv_decorate1(&g->s, id, SpvDecArrayStride, (u32)t->stride * 4);
        break;
    case T_LIST: {
        u32 arr = spv_type_array(&g->s, bty(g, t->elem), t->n);
        spv_decorate1(&g->s, arr, SpvDecArrayStride, (u32)t->stride * 4);
        u32 m[2] = {g->tint, arr};
        id = spv_type_struct(&g->s, m, 2);
        spv_member_decorate(&g->s, id, 0, SpvDecOffset, 0, true);
        spv_member_decorate(&g->s, id, 1, SpvDecOffset, (u32)t->data_off * 4, true);
        t->spv_buf_aux = arr;
        break;
    }
    default: id = ty(g, t); break;
    }
    t->spv_buf_id = id;
    return id;
}

static u32 ptr(Gen *g, u32 sc, u32 type) { return spv_type_ptr(&g->s, sc, type); }
static u32 ci(Gen *g, i32 v) { return spv_const_int(&g->s, v); }
static u32 cf(Gen *g, float v) { return spv_const_float(&g->s, v); }
static u32 cnull(Gen *g, Type *t) { return spv_const_null(&g->s, ty(g, t)); }

static u32 op1(Gen *g, u32 op, u32 type, u32 a) { return spv_op1(&g->s, op, type, a); }
static u32 op2(Gen *g, u32 op, u32 type, u32 a, u32 b) { return spv_op2(&g->s, op, type, a, b); }
static u32 ext1(Gen *g, u32 type, u32 inst, u32 a) { return spv_ext(&g->s, type, inst, &a, 1); }
static u32 ext2(Gen *g, u32 type, u32 inst, u32 a, u32 b) { u32 o[2] = {a, b}; return spv_ext(&g->s, type, inst, o, 2); }
static u32 ext3(Gen *g, u32 type, u32 inst, u32 a, u32 b, u32 c) { u32 o[3] = {a, b, c}; return spv_ext(&g->s, type, inst, o, 3); }
static u32 extract(Gen *g, u32 type, u32 v, u32 idx) { return op2(g, SpvOpCompositeExtract, type, v, idx); }
static u32 construct(Gen *g, u32 type, const u32 *parts, int n) { return spv_op(&g->s, SpvOpCompositeConstruct, type, parts, n); }
static u32 vec2(Gen *g, u32 x, u32 y) { u32 p[2] = {x, y}; return construct(g, g->tvec2, p, 2); }
static u32 load(Gen *g, u32 type, u32 p) { return spv_load(&g->s, type, p); }
static void store(Gen *g, u32 p, u32 v) { spv_store(&g->s, p, v); }
static u32 local_var(Gen *g, Type *t) { return spv_local_var(&g->s, ptr(g, SpvSCFunction, ty(g, t)), cnull(g, t)); }
static u32 local_var_id(Gen *g, u32 type) { return spv_local_var(&g->s, ptr(g, SpvSCFunction, type), 0); }
static u32 new_label(Gen *g) { return spv_new_id(&g->s); }
static void label(Gen *g, u32 l) { spv_label(&g->s, l); g->dead = false; }
static void branch(Gen *g, u32 l) { spv_branch(&g->s, l); }
static u32 g_access(Gen *g, u32 ptype, u32 base, const u32 *idx, int n) {
    u32 o[8] = {base};
    for (int i = 0; i < n; i++) o[1 + i] = idx[i];
    return spv_op(&g->s, SpvOpAccessChain, ptype, o, 1 + n);
}

static u32 sym_sc(Sym *s) { return s->kind == SYM_LOCAL ? SpvSCFunction : SpvSCPrivate; }

/* ---- constants ----------------------------------------------------------- */
static u32 const_value(Gen *g, Type *t, const float *v) {
    switch (t->kind) {
    case T_INT: return ci(g, (i32)v[0]);
    case T_REAL: return real_const(g, v[0]);
    case T_BOOL: return spv_const_bool(&g->s, v[0] != 0);
    case T_COMPLEX:
        if (g->df) {
            u32 p[4] = {cf(g, v[0]), cf(g, 0), cf(g, v[1]), cf(g, 0)};
            return spv_const_composite(&g->s, g->tvec4, p, 4);
        } else {
            u32 p[2] = {cf(g, v[0]), cf(g, v[1])};
            return spv_const_composite(&g->s, g->tvec2, p, 2);
        }
    case T_QUAT: case T_PROJ: {
        u32 p[4] = {cf(g, v[0]), cf(g, v[1]), cf(g, v[2]), cf(g, v[3])};
        return spv_const_composite(&g->s, g->tvec4, p, 4);
    }
    case T_VEC: {
        u32 p[4];
        int n = FS_MIN(t->n, 4);
        for (int i = 0; i < n; i++) p[i] = const_value(g, t->elem, v + i);
        return spv_const_composite(&g->s, ty(g, t), p, n);
    }
    default: return cnull(g, t);
    }
}

static u32 one_of(Gen *g, Type *t) {
    float v[4] = {1, 0, 0, 0};
    return const_value(g, t, v);
}

/* ---- double-single ("df") arithmetic, for deep zooms ---------------------------------------
 * In a viewer compiled for deep zooms (codegen_viewer_df), a real number is a vec2 (hi, lo) with
 * hi = hi + lo rounded to a float: about 48 bits of mantissa instead of 24.  A complex number is a
 * vec4 (re.hi, re.lo, im.hi, im.lo).  The error-free transformations below rely on exact IEEE
 * float operations: their results are decorated NoContraction (no fused or reassociated maths;
 * SPIRV-Cross keeps them exact in Metal's fast math too).  Elementary functions other than sqrt are
 * computed in single precision (they serve colours, not coordinates). */
enum { SpvDecNoContraction = 42, GLSLInverseSqrt = 32 };

static u32 xop(Gen *g, u32 op, u32 a, u32 b) { /* an exact float operation */
    u32 r = op2(g, op, g->tfloat, a, b);
    spv_decorate(&g->s, r, SpvDecNoContraction, NULL, 0);
    return r;
}
static u32 df_hi(Gen *g, u32 a) { return extract(g, g->tfloat, a, 0); }
static u32 df_lo(Gen *g, u32 a) { return extract(g, g->tfloat, a, 1); }
static u32 df_from_f(Gen *g, u32 x) { return vec2(g, x, cf(g, 0)); }
static u32 df_neg(Gen *g, u32 a) { return op1(g, SpvOpFNegate, g->tvec2, a); }
static u32 bvecn(Gen *g, u32 c, int n) {
    u32 p[4] = {c, c, c, c};
    return construct(g, spv_type_vec(&g->s, g->tbool, (u32)n), p, n);
}
static u32 sel2(Gen *g, u32 c, u32 a, u32 b) { return spv_op3(&g->s, SpvOpSelect, g->tvec2, bvecn(g, c, 2), a, b); }
static u32 sel4(Gen *g, u32 c, u32 a, u32 b) { return spv_op3(&g->s, SpvOpSelect, g->tvec4, bvecn(g, c, 4), a, b); }

/* s + e = a + b exactly (Knuth's two-sum) */
static void two_sum(Gen *g, u32 a, u32 b, u32 *s, u32 *e) {
    *s = xop(g, SpvOpFAdd, a, b);
    u32 bb = xop(g, SpvOpFSub, *s, a);
    u32 ea = xop(g, SpvOpFSub, a, xop(g, SpvOpFSub, *s, bb));
    *e = xop(g, SpvOpFAdd, ea, xop(g, SpvOpFSub, b, bb));
}
/* s + e = a + b exactly, when |a| >= |b| */
static void quick_two_sum(Gen *g, u32 a, u32 b, u32 *s, u32 *e) {
    *s = xop(g, SpvOpFAdd, a, b);
    *e = xop(g, SpvOpFSub, b, xop(g, SpvOpFSub, *s, a));
}
/* a = hi + lo with 12-bit halves (Dekker), so that products of halves are exact */
static void df_split(Gen *g, u32 a, u32 *hi, u32 *lo) {
    u32 c = xop(g, SpvOpFMul, cf(g, 4097.0f), a);
    *hi = xop(g, SpvOpFSub, c, xop(g, SpvOpFSub, c, a));
    *lo = xop(g, SpvOpFSub, a, *hi);
}
/* p + e = a b exactly (Dekker's product: a fused multiply-add isn't guaranteed to be fused) */
static void two_prod(Gen *g, u32 a, u32 b, u32 *p, u32 *e) {
    *p = xop(g, SpvOpFMul, a, b);
    u32 ah, al, bh, bl;
    df_split(g, a, &ah, &al);
    df_split(g, b, &bh, &bl);
    u32 t = xop(g, SpvOpFSub, xop(g, SpvOpFMul, ah, bh), *p);
    t = xop(g, SpvOpFAdd, t, xop(g, SpvOpFMul, ah, bl));
    t = xop(g, SpvOpFAdd, t, xop(g, SpvOpFMul, al, bh));
    *e = xop(g, SpvOpFAdd, t, xop(g, SpvOpFMul, al, bl));
}
static u32 df_add(Gen *g, u32 a, u32 b) { /* the accurate sum (also when the parts cancel) */
    u32 s, e, t, f;
    two_sum(g, df_hi(g, a), df_hi(g, b), &s, &e);
    two_sum(g, df_lo(g, a), df_lo(g, b), &t, &f);
    e = xop(g, SpvOpFAdd, e, t);
    quick_two_sum(g, s, e, &s, &e);
    e = xop(g, SpvOpFAdd, e, f);
    quick_two_sum(g, s, e, &s, &e);
    return vec2(g, s, e);
}
static u32 df_sub(Gen *g, u32 a, u32 b) { return df_add(g, a, df_neg(g, b)); }
static u32 df_add_f(Gen *g, u32 a, u32 b) { return df_add(g, a, df_from_f(g, b)); }
static u32 df_mul(Gen *g, u32 a, u32 b) {
    u32 p, e;
    two_prod(g, df_hi(g, a), df_hi(g, b), &p, &e);
    u32 c = xop(g, SpvOpFAdd, xop(g, SpvOpFMul, df_hi(g, a), df_lo(g, b)), xop(g, SpvOpFMul, df_lo(g, a), df_hi(g, b)));
    e = xop(g, SpvOpFAdd, e, c);
    quick_two_sum(g, p, e, &p, &e);
    return vec2(g, p, e);
}
static u32 df_mul_f(Gen *g, u32 a, u32 b) { /* df times float */
    u32 p, e;
    two_prod(g, df_hi(g, a), b, &p, &e);
    e = xop(g, SpvOpFAdd, e, xop(g, SpvOpFMul, df_lo(g, a), b));
    quick_two_sum(g, p, e, &p, &e);
    return vec2(g, p, e);
}
static u32 df_div(Gen *g, u32 a, u32 b) { /* long division: three float quotients */
    u32 bh = df_hi(g, b);
    u32 q1 = op2(g, SpvOpFDiv, g->tfloat, df_hi(g, a), bh);
    u32 r = df_sub(g, a, df_mul_f(g, b, q1));
    u32 q2 = op2(g, SpvOpFDiv, g->tfloat, df_hi(g, r), bh);
    r = df_sub(g, r, df_mul_f(g, b, q2));
    u32 q3 = op2(g, SpvOpFDiv, g->tfloat, df_hi(g, r), bh);
    u32 s, e;
    quick_two_sum(g, q1, q2, &s, &e);
    return df_add_f(g, vec2(g, s, e), q3);
}
static u32 df_sqrt(Gen *g, u32 a) { /* Karp's trick: y = sqrt(hi), then one Newton correction */
    u32 ah = df_hi(g, a);
    u32 pos = op2(g, SpvOpFOrdGreaterThan, g->tbool, ah, cf(g, 0));
    u32 x = ext1(g, g->tfloat, GLSLInverseSqrt, ext2(g, g->tfloat, GLSLFMax, ah, cf(g, 1e-37f)));
    u32 y = xop(g, SpvOpFMul, ah, x);
    u32 p, e;
    two_prod(g, y, y, &p, &e);
    u32 d = df_sub(g, a, vec2(g, p, e));
    u32 corr = xop(g, SpvOpFMul, xop(g, SpvOpFMul, df_hi(g, d), x), cf(g, 0.5f));
    u32 s, ee;
    quick_two_sum(g, y, corr, &s, &ee);
    return sel2(g, pos, vec2(g, s, ee), df_from_f(g, ext1(g, g->tfloat, GLSLSqrt, ah))); /* 0, or NaN below 0 */
}
static u32 df_cmp(Gen *g, int op, u32 a, u32 b) { /* op: TK_LT, TK_LE, TK_GT, TK_GE */
    u32 B = g->tbool, ah = df_hi(g, a), bh = df_hi(g, b), al = df_lo(g, a), bl = df_lo(g, b);
    u32 eqh = op2(g, SpvOpFOrdEqual, B, ah, bh);
    u32 hi_op = op == TK_LT || op == TK_LE ? SpvOpFOrdLessThan : SpvOpFOrdGreaterThan;
    u32 lo_op = op == TK_LT ? SpvOpFOrdLessThan : op == TK_LE ? SpvOpFOrdLessThanEqual
              : op == TK_GT ? SpvOpFOrdGreaterThan : SpvOpFOrdGreaterThanEqual;
    return op2(g, SpvOpLogicalOr, B, op2(g, hi_op, B, ah, bh), op2(g, SpvOpLogicalAnd, B, eqh, op2(g, lo_op, B, al, bl)));
}
static u32 df_floor(Gen *g, u32 a) {
    u32 ah = df_hi(g, a), fh = ext1(g, g->tfloat, GLSLFloor, ah);
    u32 is_int = op2(g, SpvOpFOrdEqual, g->tbool, fh, ah);
    u32 s, e;
    quick_two_sum(g, fh, ext1(g, g->tfloat, GLSLFloor, df_lo(g, a)), &s, &e);
    return sel2(g, is_int, vec2(g, s, e), df_from_f(g, fh));
}
static u32 df_ceil(Gen *g, u32 a) { return df_neg(g, df_floor(g, df_neg(g, a))); }
static u32 df_abs(Gen *g, u32 a) {
    return sel2(g, op2(g, SpvOpFOrdLessThan, g->tbool, df_hi(g, a), cf(g, 0)), df_neg(g, a), a);
}
static u32 df_mod(Gen *g, u32 a, u32 b) { return df_sub(g, a, df_mul(g, b, df_floor(g, df_div(g, a, b)))); }
/* the integer part, clamped to 32 bits (after floor / ceil / trunc / round) */
static u32 df_to_int(Gen *g, u32 a) {
    u32 lim[2] = {cf(g, -2147483648.0f), cf(g, 2147483520.0f)};
    u32 h = op1(g, SpvOpConvertFToS, g->tint, ext3(g, g->tfloat, GLSLFClamp, df_hi(g, a), lim[0], lim[1]));
    u32 l = op1(g, SpvOpConvertFToS, g->tint, ext3(g, g->tfloat, GLSLFClamp, df_lo(g, a), lim[0], lim[1]));
    return op2(g, SpvOpIAdd, g->tint, h, l);
}
/* an int exactly: hi = float(i), lo = the rest */
static u32 df_from_int(Gen *g, u32 i) {
    u32 h = op1(g, SpvOpConvertSToF, g->tfloat, i);
    u32 hi_int = op1(g, SpvOpConvertFToS, g->tint, ext3(g, g->tfloat, GLSLFClamp, h, cf(g, -2147483648.0f), cf(g, 2147483520.0f)));
    return vec2(g, h, op1(g, SpvOpConvertSToF, g->tfloat, op2(g, SpvOpISub, g->tint, i, hi_int)));
}
/* complex df: vec4 (re.hi, re.lo, im.hi, im.lo) */
static u32 shuffle(Gen *g, u32 type, u32 a, u32 b, const u32 *comp, int n) {
    u32 o[6] = {a, b};
    for (int i = 0; i < n; i++) o[2 + i] = comp[i];
    return spv_op(&g->s, SpvOpVectorShuffle, type, o, 2 + n);
}
static u32 c_re(Gen *g, u32 z) { static const u32 c[2] = {0, 1}; return shuffle(g, g->tvec2, z, z, c, 2); }
static u32 c_im(Gen *g, u32 z) { static const u32 c[2] = {2, 3}; return shuffle(g, g->tvec2, z, z, c, 2); }
static u32 c_make(Gen *g, u32 re, u32 im) { static const u32 c[4] = {0, 1, 2, 3}; return shuffle(g, g->tvec4, re, im, c, 4); }
static u32 dc_mul(Gen *g, u32 a, u32 b) {
    u32 ar = c_re(g, a), ai = c_im(g, a), br = c_re(g, b), bi = c_im(g, b);
    return c_make(g, df_sub(g, df_mul(g, ar, br), df_mul(g, ai, bi)), df_add(g, df_mul(g, ar, bi), df_mul(g, ai, br)));
}
static u32 dc_div(Gen *g, u32 a, u32 b) {
    u32 ar = c_re(g, a), ai = c_im(g, a), br = c_re(g, b), bi = c_im(g, b);
    u32 d = df_add(g, df_mul(g, br, br), df_mul(g, bi, bi));
    u32 re = df_add(g, df_mul(g, ar, br), df_mul(g, ai, bi)), im = df_sub(g, df_mul(g, ai, br), df_mul(g, ar, bi));
    return c_make(g, df_div(g, re, d), df_div(g, im, d));
}
/* a df constant from a double: hi + lo */
static u32 df_const(Gen *g, double v) {
    float hi = (float)v, lo = (float)(v - (double)hi);
    u32 p[2] = {cf(g, hi), cf(g, lo)};
    return spv_const_composite(&g->s, g->tvec2, p, 2);
}
static u32 real_const(Gen *g, double v) { return g->df ? df_const(g, v) : cf(g, (float)v); }

/* ---- number arithmetic ----------------------------------------------------- */
static u32 g_cmul(Gen *g, u32 a, u32 b) {
    u32 f = g->tfloat;
    u32 ax = extract(g, f, a, 0), ay = extract(g, f, a, 1), bx = extract(g, f, b, 0), by = extract(g, f, b, 1);
    u32 re = op2(g, SpvOpFSub, f, op2(g, SpvOpFMul, f, ax, bx), op2(g, SpvOpFMul, f, ay, by));
    u32 im = op2(g, SpvOpFAdd, f, op2(g, SpvOpFMul, f, ax, by), op2(g, SpvOpFMul, f, ay, bx));
    return vec2(g, re, im);
}
static u32 g_cdiv(Gen *g, u32 a, u32 b) {
    u32 f = g->tfloat;
    u32 ax = extract(g, f, a, 0), ay = extract(g, f, a, 1), bx = extract(g, f, b, 0), by = extract(g, f, b, 1);
    u32 d = op2(g, SpvOpFAdd, f, op2(g, SpvOpFMul, f, bx, bx), op2(g, SpvOpFMul, f, by, by));
    u32 re = op2(g, SpvOpFAdd, f, op2(g, SpvOpFMul, f, ax, bx), op2(g, SpvOpFMul, f, ay, by));
    u32 im = op2(g, SpvOpFSub, f, op2(g, SpvOpFMul, f, ay, bx), op2(g, SpvOpFMul, f, ax, by));
    return vec2(g, op2(g, SpvOpFDiv, f, re, d), op2(g, SpvOpFDiv, f, im, d));
}
static u32 g_qmul(Gen *g, u32 a, u32 b) {
    u32 f = g->tfloat, A[4], B[4];
    for (int i = 0; i < 4; i++) { A[i] = extract(g, f, a, (u32)i); B[i] = extract(g, f, b, (u32)i); }
#define M(i, j) op2(g, SpvOpFMul, f, A[i], B[j])
#define ADD(x, y) op2(g, SpvOpFAdd, f, x, y)
#define SUB(x, y) op2(g, SpvOpFSub, f, x, y)
    u32 r[4];
    r[0] = SUB(SUB(SUB(M(0, 0), M(1, 1)), M(2, 2)), M(3, 3));
    r[1] = SUB(ADD(ADD(M(0, 1), M(1, 0)), M(2, 3)), M(3, 2));
    r[2] = ADD(ADD(SUB(M(0, 2), M(1, 3)), M(2, 0)), M(3, 1));
    r[3] = ADD(SUB(ADD(M(0, 3), M(1, 2)), M(2, 1)), M(3, 0));
#undef M
#undef ADD
#undef SUB
    return construct(g, g->tvec4, r, 4);
}
static u32 g_qinv(Gen *g, u32 a) {
    u32 f = g->tfloat;
    u32 d = op2(g, SpvOpDot, f, a, a), r[4];
    for (int i = 0; i < 4; i++) {
        u32 x = extract(g, f, a, (u32)i);
        if (i) x = op1(g, SpvOpFNegate, f, x);
        r[i] = op2(g, SpvOpFDiv, f, x, d);
    }
    return construct(g, g->tvec4, r, 4);
}

/* a op b for two values of the same number type t */
static u32 num_op_g(Gen *g, Type *t, int op, u32 a, u32 b) {
    u32 tid = ty(g, t);
    if (t->kind == T_INT) {
        switch (op) {
        case TK_PLUS: return op2(g, SpvOpIAdd, tid, a, b);
        case TK_MINUS: return op2(g, SpvOpISub, tid, a, b);
        case TK_STAR: return op2(g, SpvOpIMul, tid, a, b);
        case TK_MOD: {
            u32 z = op2(g, SpvOpIEqual, g->tbool, b, ci(g, 0));
            u32 m1 = op2(g, SpvOpIEqual, g->tbool, b, ci(g, -1));
            u32 bad = op2(g, SpvOpLogicalOr, g->tbool, z, m1);
            u32 safe_b = spv_op3(&g->s, SpvOpSelect, tid, bad, ci(g, 1), b);
            u32 m = op2(g, SpvOpSMod, tid, a, safe_b);
            return spv_op3(&g->s, SpvOpSelect, tid, bad, ci(g, 0), m);
        }
        }
        return ci(g, 0);
    }
    if (g->df && t->kind == T_REAL) {
        switch (op) {
        case TK_PLUS: return df_add(g, a, b);
        case TK_MINUS: return df_sub(g, a, b);
        case TK_STAR: return df_mul(g, a, b);
        case TK_SLASH: return df_div(g, a, b);
        case TK_MOD: return df_mod(g, a, b);
        }
        return cnull(g, t);
    }
    if (g->df && t->kind == T_COMPLEX) {
        switch (op) {
        case TK_PLUS: case TK_MINUS: case TK_MOD:
            return c_make(g, num_op_g(g, t_real, op, c_re(g, a), c_re(g, b)), num_op_g(g, t_real, op, c_im(g, a), c_im(g, b)));
        case TK_STAR: return dc_mul(g, a, b);
        case TK_SLASH: return dc_div(g, a, b);
        }
        return cnull(g, t);
    }
    switch (op) {
    case TK_PLUS: return op2(g, SpvOpFAdd, tid, a, b);
    case TK_MINUS: return op2(g, SpvOpFSub, tid, a, b);
    case TK_MOD: return op2(g, SpvOpFMod, tid, a, b);
    case TK_STAR:
        if (t->kind == T_REAL) return op2(g, SpvOpFMul, tid, a, b);
        if (t->kind == T_COMPLEX) return g_cmul(g, a, b);
        return g_qmul(g, a, b);
    case TK_SLASH:
        if (t->kind == T_REAL) return op2(g, SpvOpFDiv, tid, a, b);
        if (t->kind == T_COMPLEX) return g_cdiv(g, a, b);
        return g_qmul(g, a, g_qinv(g, b));
    }
    return cnull(g, t);
}

static u32 norm2_g(Gen *g, Type *t, u32 v) {
    u32 f = g->tfloat;
    if (g->df) { /* a df real */
        switch (t->kind) {
        case T_INT: { u32 x = df_from_int(g, v); return df_mul(g, x, x); }
        case T_REAL: return df_mul(g, v, v);
        case T_COMPLEX: { u32 re = c_re(g, v), im = c_im(g, v); return df_add(g, df_mul(g, re, re), df_mul(g, im, im)); }
        case T_QUAT: return df_from_f(g, op2(g, SpvOpDot, f, v, v));
        case T_VEC: {
            u32 acc = df_const(g, 0);
            for (int i = 0; i < t->n; i++) acc = df_add(g, acc, norm2_g(g, t->elem, extract(g, ty(g, t->elem), v, (u32)i)));
            return acc;
        }
        default: return df_const(g, 0);
        }
    }
    switch (t->kind) {
    case T_INT: { u32 x = op1(g, SpvOpConvertSToF, f, v); return op2(g, SpvOpFMul, f, x, x); }
    case T_REAL: return op2(g, SpvOpFMul, f, v, v);
    case T_COMPLEX: case T_QUAT: return op2(g, SpvOpDot, f, v, v);
    case T_VEC: {
        u32 acc = cf(g, 0);
        for (int i = 0; i < t->n; i++)
            acc = op2(g, SpvOpFAdd, f, acc, norm2_g(g, t->elem, extract(g, ty(g, t->elem), v, (u32)i)));
        return acc;
    }
    default: return cf(g, 0);
    }
}

static u32 global_value(Gen *g, Sym *s) { return load(g, ty(g, s->type), s->spv_var); }

static u32 approx_zero(Gen *g, u32 n2) {
    if (g->df) n2 = df_hi(g, n2);
    u32 eps = global_value(g, g->prog->s_epsilon);
    if (g->df) eps = df_hi(g, eps);
    u32 eps2 = op2(g, SpvOpFMul, g->tfloat, eps, eps);
    u32 lt = op2(g, SpvOpFOrdLessThan, g->tbool, n2, eps2);
    u32 z = op2(g, SpvOpFOrdEqual, g->tbool, n2, cf(g, 0));
    return op2(g, SpvOpLogicalOr, g->tbool, lt, z);
}

static u32 negate(Gen *g, Type *t, u32 v) {
    if (t->kind == T_INT) return op1(g, SpvOpSNegate, g->tint, v);
    if (t->kind == T_VEC) {
        u32 parts[64];
        for (int i = 0; i < t->n; i++) parts[i] = negate(g, t->elem, extract(g, ty(g, t->elem), v, (u32)i));
        return construct(g, ty(g, t), parts, t->n);
    }
    return op1(g, SpvOpFNegate, ty(g, t), v);
}

static u32 convert_g(Gen *g, Type *from, Type *to, u32 v) {
    if (from == to) return v;
    if (to->kind == T_PROJ && type_is_number(from) && from->kind != T_QUAT) { /* z -> [z : 1] (the checker lowers
                                                                                     these; just in case) */
        u32 z = convert_g(g, from, t_complex, v), f = g->tfloat;
        u32 x = g->df ? df_hi(g, c_re(g, z)) : extract(g, f, z, 0), y = g->df ? df_hi(g, c_im(g, z)) : extract(g, f, z, 1);
        u32 p[4] = {x, y, cf(g, 1), cf(g, 0)};
        return proj_scale(g, construct(g, g->tvec4, p, 4));
    }
    if (g->df && type_is_number(from) && type_is_number(to)) {
        u32 f = g->tfloat, x, y;
        switch (from->kind) {
        case T_INT: x = df_from_int(g, v); y = df_const(g, 0); break;
        case T_REAL: x = v; y = df_const(g, 0); break;
        case T_COMPLEX: x = c_re(g, v); y = c_im(g, v); break;
        default: x = df_from_f(g, extract(g, f, v, 0)); y = df_from_f(g, extract(g, f, v, 1)); break;
        }
        if (to->kind == T_REAL) return x;
        if (to->kind == T_COMPLEX) return c_make(g, x, y);
        if (from->kind == T_QUAT) return v;
        u32 p[4] = {df_hi(g, x), df_hi(g, y), cf(g, 0), cf(g, 0)};
        return construct(g, g->tvec4, p, 4);
    }
    if (type_is_number(from) && type_is_number(to)) {
        u32 f = g->tfloat;
        u32 x = from->kind == T_INT ? op1(g, SpvOpConvertSToF, f, v) : from->kind == T_REAL ? v : extract(g, f, v, 0);
        if (to->kind == T_REAL) return x;
        u32 y = from->kind == T_COMPLEX || from->kind == T_QUAT ? extract(g, f, v, 1) : cf(g, 0);
        if (to->kind == T_COMPLEX) return vec2(g, x, y);
        u32 p[4] = {x, y, cf(g, 0), cf(g, 0)};
        return construct(g, g->tvec4, p, 4);
    }
    if (from->kind == T_VEC && to->kind == T_VEC) {
        u32 parts[64];
        for (int i = 0; i < to->n; i++)
            parts[i] = convert_g(g, from->elem, to->elem, extract(g, ty(g, from->elem), v, (u32)i));
        return construct(g, ty(g, to), parts, to->n);
    }
    return cnull(g, to);
}

/* list value from items (already of the element type) */
static u32 make_list(Gen *g, Type *lt, const u32 *items, int n) {
    /* the logical list struct is {int, array<elem, cap>} where the array is ty(elem^cap) */
    u32 *parts = (u32 *)xmalloc(sizeof(u32) * (size_t)lt->n);
    for (int i = 0; i < lt->n; i++) parts[i] = i < n ? items[i] : cnull(g, lt->elem);
    u32 arr_type = ty(g, type_vec(lt->elem, lt->n));
    u32 arr = construct(g, arr_type, parts, lt->n);
    free(parts);
    u32 m[2] = {ci(g, n), arr};
    return construct(g, ty(g, lt), m, 2);
}

/* ---- lvalues -------------------------------------------------------------- */
/* see clamp_index in interp.c: -1 is the last item (hi) */
static u32 g_clamp_index(Gen *g, u32 k, u32 lo, u32 hi) {
    u32 neg = op2(g, SpvOpSLessThan, g->tbool, k, ci(g, 0));
    k = spv_op3(&g->s, SpvOpSelect, g->tint, neg, op2(g, SpvOpIAdd, g->tint, k, op2(g, SpvOpIAdd, g->tint, hi, ci(g, 1))), k);
    u32 c = ext3(g, g->tint, GLSLSClamp, k, lo, hi);
    return op2(g, SpvOpISub, g->tint, c, ci(g, 1));
}

/* pointer to an lvalue; *sc receives its storage class */
static u32 lv_ptr(Gen *g, Expr *e, u32 *sc) {
    if (e->kind == E_NAME || e->kind == E_INTERNAL || e->kind == E_MEMBER) {
        *sc = sym_sc(e->sym);
        return e->sym->spv_var;
    }
    /* E_INDEX */
    u32 base = lv_ptr(g, e->a, sc);
    u32 k = gen_expr(g, e->b);
    Type *bt = e->a->type;
    switch (bt->kind) {
    case T_VEC: {
        u32 i = g_clamp_index(g, k, ci(g, 1), ci(g, bt->n));
        return g_access(g, ptr(g, *sc, ty(g, bt->elem)), base, &i, 1);
    }
    case T_COMPLEX: case T_QUAT: {
        u32 i = g_clamp_index(g, k, ci(g, 1), ci(g, bt->kind == T_COMPLEX ? 2 : 4));
        return g_access(g, ptr(g, *sc, g->tfloat), base, &i, 1);
    }
    case T_LIST: {
        u32 z = ci(g, 0);
        u32 len = load(g, g->tint, g_access(g, ptr(g, *sc, g->tint), base, &z, 1));
        u32 hi = ext2(g, g->tint, GLSLSMax, len, ci(g, 1));
        u32 idx[2] = {ci(g, 1), g_clamp_index(g, k, ci(g, 1), hi)};
        return g_access(g, ptr(g, *sc, ty(g, bt->elem)), base, idx, 2);
    }
    default: return base;
    }
}

static bool is_lvalue_expr(Expr *e) {
    if (e->kind == E_NAME || e->kind == E_INTERNAL || e->kind == E_MEMBER)
        return e->sym && e->sym->kind != SYM_CONST && e->sym->spv_var != 0;
    if (e->kind == E_INDEX) return is_lvalue_expr(e->a);
    return false;
}

static u32 gen_index(Gen *g, Expr *e) {
    Type *bt = e->a->type;
    u32 sc, base;
    if (is_lvalue_expr(e->a)) {
        base = lv_ptr(g, e->a, &sc);
    } else {
        u32 v = gen_expr(g, e->a);
        base = local_var(g, bt);
        store(g, base, v);
        sc = SpvSCFunction;
    }
    u32 k = gen_expr(g, e->b);
    if (g->df && bt->kind == T_COMPLEX) { /* z[1] = re z, z[2] = im z */
        u32 z = load(g, g->tvec4, base);
        u32 second = op2(g, SpvOpIEqual, g->tbool, g_clamp_index(g, k, ci(g, 1), ci(g, 2)), ci(g, 1));
        return sel2(g, second, c_im(g, z), c_re(g, z));
    }
    switch (bt->kind) {
    case T_VEC: {
        u32 i = g_clamp_index(g, k, ci(g, 1), ci(g, bt->n));
        return load(g, ty(g, bt->elem), g_access(g, ptr(g, sc, ty(g, bt->elem)), base, &i, 1));
    }
    case T_COMPLEX: case T_QUAT: {
        u32 i = g_clamp_index(g, k, ci(g, 1), ci(g, bt->kind == T_COMPLEX ? 2 : 4));
        u32 x = load(g, g->tfloat, g_access(g, ptr(g, sc, g->tfloat), base, &i, 1));
        return g->df ? df_from_f(g, x) : x; /* (quaternions stay in single precision) */
    }
    case T_LIST: {
        u32 z = ci(g, 0);
        u32 len = load(g, g->tint, g_access(g, ptr(g, sc, g->tint), base, &z, 1));
        u32 hi = ext2(g, g->tint, GLSLSMax, len, ci(g, 1));
        u32 idx[2] = {ci(g, 1), g_clamp_index(g, k, ci(g, 1), hi)};
        u32 v = load(g, ty(g, bt->elem), g_access(g, ptr(g, sc, ty(g, bt->elem)), base, idx, 2));
        u32 nonempty = op2(g, SpvOpSGreaterThan, g->tbool, len, ci(g, 0));
        u32 et = ty(g, bt->elem);
        if (g->df && bt->elem->kind == T_REAL) return sel2(g, nonempty, v, cnull(g, bt->elem));
        if (g->df && bt->elem->kind == T_COMPLEX) return sel4(g, nonempty, v, cnull(g, bt->elem));
        if (bt->elem->kind == T_COMPLEX || bt->elem->kind == T_QUAT) {
            /* OpSelect on vectors needs a vector condition in SPIR-V 1.0 */
            int n = bt->elem->kind == T_COMPLEX ? 2 : 4;
            u32 c[4] = {nonempty, nonempty, nonempty, nonempty};
            u32 bvec = construct(g, spv_type_vec(&g->s, g->tbool, (u32)n), c, n);
            return spv_op3(&g->s, SpvOpSelect, et, bvec, v, cnull(g, bt->elem));
        }
        return spv_op3(&g->s, SpvOpSelect, et, nonempty, v, cnull(g, bt->elem));
    }
    default: return cnull(g, e->type);
    }
}

/* ---- calls --------------------------------------------------------------- */
static void queue_func(Gen *g, Func *f) {
    if (f->spv_fn) return;
    f->spv_fn = spv_new_id(&g->s);
    vec_push(g->queue, f);
}

static u32 f2i_clamped(Gen *g, u32 x) {
    u32 c = ext3(g, g->tfloat, GLSLFClamp, x, cf(g, -2147483648.0f), cf(g, 2147483520.0f));
    return op1(g, SpvOpConvertFToS, g->tint, c);
}

/* a point of the sphere (a vec4 (a, b)) scaled so that its largest component is 1 (as proj_make in interp.c) */
static u32 proj_scale(Gen *g, u32 v) {
    u32 f = g->tfloat;
    u32 av = ext1(g, g->tvec4, GLSLFAbs, v);
    u32 m = ext2(g, f, GLSLFMax, ext2(g, f, GLSLFMax, extract(g, f, av, 0), extract(g, f, av, 1)),
                 ext2(g, f, GLSLFMax, extract(g, f, av, 2), extract(g, f, av, 3)));
    u32 s = op2(g, SpvOpFDiv, f, cf(g, 1), ext2(g, f, GLSLFMax, m, cf(g, 1e-30f)));
    return op2(g, SpvOpVectorTimesScalar, g->tvec4, v, s);
}

/* the intrinsics on reals in double-single precision (0: not one of them) */
static u32 gen_intrinsic_df(Gen *g, Expr *e, const u32 *a) {
    u32 f = g->tfloat, B = g->tbool;
    int id = e->intrinsic;
    u32 h0 = 0, h1 = 0, h2 = 0;
    Type *t0 = e->nargs > 0 ? e->args[0]->type : NULL;
    if (t0 && t0->kind == T_REAL) h0 = df_hi(g, a[0]);
    if (e->nargs > 1 && e->args[1]->type->kind == T_REAL) h1 = df_hi(g, a[1]);
    if (e->nargs > 2 && e->args[2]->type->kind == T_REAL) h2 = df_hi(g, a[2]);
    switch (id) { /* single precision on the high part */
    case I_SIN: return df_from_f(g, ext1(g, f, GLSLSin, h0));
    case I_COS: return df_from_f(g, ext1(g, f, GLSLCos, h0));
    case I_TAN: return df_from_f(g, ext1(g, f, GLSLTan, h0));
    case I_ASIN: return df_from_f(g, ext1(g, f, GLSLAsin, h0));
    case I_ACOS: return df_from_f(g, ext1(g, f, GLSLAcos, h0));
    case I_ATAN: return df_from_f(g, ext1(g, f, GLSLAtan, h0));
    case I_SINH: return df_from_f(g, ext1(g, f, GLSLSinh, h0));
    case I_COSH: return df_from_f(g, ext1(g, f, GLSLCosh, h0));
    case I_TANH: return df_from_f(g, ext1(g, f, GLSLTanh, h0));
    case I_ASINH: return df_from_f(g, ext1(g, f, GLSLAsinh, h0));
    case I_ACOSH: return df_from_f(g, ext1(g, f, GLSLAcosh, h0));
    case I_ATANH: return df_from_f(g, ext1(g, f, GLSLAtanh, h0));
    case I_EXP2: return df_from_f(g, ext1(g, f, GLSLExp2, h0));
    case I_ATAN2: return df_from_f(g, ext2(g, f, GLSLAtan2, h0, h1));
    case I_POW: return df_from_f(g, ext2(g, f, GLSLPow, h0, h1));
    case I_SMOOTHSTEP: return df_from_f(g, ext3(g, f, GLSLSmoothStep, h0, h1, h2));
    case I_ISNAN: return op1(g, SpvOpIsNan, B, h0);
    case I_ISINF: return op1(g, SpvOpIsInf, B, h0);
    case I_EXP: { /* exp(hi + lo) = exp(hi) (1 + lo) */
        u32 x = ext1(g, f, GLSLExp, h0), s, err;
        quick_two_sum(g, x, op2(g, SpvOpFMul, f, x, df_lo(g, a[0])), &s, &err);
        return vec2(g, s, err);
    }
    case I_LOG: case I_LOG2: { /* log(hi + lo) = log(hi) + lo / hi */
        u32 l = ext1(g, f, GLSLLog, h0), c = op2(g, SpvOpFDiv, f, df_lo(g, a[0]), h0), s, err;
        if (id == I_LOG2) {
            l = ext1(g, f, GLSLLog2, h0);
            c = op2(g, SpvOpFMul, f, c, cf(g, 1.44269504f));
        }
        quick_two_sum(g, l, c, &s, &err);
        return vec2(g, s, err);
    }
    case I_SQRT: return df_sqrt(g, a[0]);
    case I_FLOOR: return df_floor(g, a[0]);
    case I_CEIL: return df_ceil(g, a[0]);
    case I_TRUNC: return sel2(g, op2(g, SpvOpFOrdLessThan, B, h0, cf(g, 0)), df_ceil(g, a[0]), df_floor(g, a[0]));
    case I_FRACT: return df_sub(g, a[0], df_floor(g, a[0]));
    case I_FABS: return df_abs(g, a[0]);
    case I_FSIGN: return df_from_f(g, ext1(g, f, GLSLFSign, h0));
    case I_FMIN: return sel2(g, df_cmp(g, TK_LT, a[1], a[0]), a[1], a[0]);
    case I_FMAX: return sel2(g, df_cmp(g, TK_GT, a[1], a[0]), a[1], a[0]);
    case I_FMOD: return df_mod(g, a[0], a[1]);
    case I_STEP: return sel2(g, df_cmp(g, TK_LT, a[1], a[0]), df_const(g, 0), df_const(g, 1)); /* step(edge, x) */
    case I_FCLAMP: {
        u32 lo = sel2(g, df_cmp(g, TK_LT, a[0], a[1]), a[1], a[0]);
        return sel2(g, df_cmp(g, TK_GT, lo, a[2]), a[2], lo);
    }
    case I_FMIX: return df_add(g, a[0], df_mul(g, df_sub(g, a[1], a[0]), a[2]));
    case I_FLOORI: return df_to_int(g, df_floor(g, a[0]));
    case I_CEILI: return df_to_int(g, df_ceil(g, a[0]));
    case I_ROUNDI: return df_to_int(g, df_floor(g, df_add(g, a[0], df_const(g, 0.5))));
    case I_TRUNCI:
        return df_to_int(g, sel2(g, op2(g, SpvOpFOrdLessThan, B, h0, cf(g, 0)), df_ceil(g, a[0]), df_floor(g, a[0])));
    case I_COMPLEX: return c_make(g, a[0], a[1]);
    case I_RE: return t0->kind == T_COMPLEX ? c_re(g, a[0]) : df_from_f(g, extract(g, f, a[0], 0));
    case I_IM: return t0->kind == T_COMPLEX ? c_im(g, a[0]) : df_from_f(g, extract(g, f, a[0], 1));
    case I_QUAT: {
        u32 p[4];
        for (int i = 0; i < 4; i++) p[i] = df_hi(g, a[i]);
        return construct(g, g->tvec4, p, 4);
    }
    case I_PMAKE: { /* (the sphere stays in single precision: the high parts) */
        u32 p[4] = {extract(g, f, a[0], 0), extract(g, f, a[0], 2), extract(g, f, a[1], 0), extract(g, f, a[1], 2)};
        return proj_scale(g, construct(g, g->tvec4, p, 4));
    }
    case I_PA: case I_PB: {
        u32 k = id == I_PA ? 0 : 2;
        return c_make(g, df_from_f(g, extract(g, f, a[0], k)), df_from_f(g, extract(g, f, a[0], k + 1)));
    }
    }
    return 0;
}

static u32 gen_intrinsic(Gen *g, Expr *e) {
    int id = e->intrinsic;
    u32 f = g->tfloat;
    if (id == I_FLOOD_SEED || id == I_FLOOD_FROM || id == I_FLOOD_OUT) {
        u32 a0 = gen_expr(g, e->args[0]);
        u32 a1 = e->nargs > 1 ? gen_expr(g, e->args[1]) : 0;
        if (!g->compute) return id == I_FLOOD_SEED ? a0 : id == I_FLOOD_FROM ? a1 : 0; /* direct solve */
        if (id == I_FLOOD_OUT) {
            /* (the field keeps solutions in single precision: they only seed the neighbours' solves) */
            store(g, g->flood_out_t_var, g->df ? vec2(g, df_hi(g, c_re(g, a0)), df_hi(g, c_im(g, a0))) : a0);
            store(g, g->flood_out_ok_var, spv_op3(&g->s, SpvOpSelect, g->tint, a1, ci(g, 1), ci(g, 0)));
            return 0;
        }
        u32 mode = load(g, g->tint, g->flood_mode_var);
        if (g->df) {
            u32 is_cont = op2(g, SpvOpIEqual, g->tbool, mode, ci(g, FLOOD_CONTINUE));
            if (id == I_FLOOD_SEED) {
                u32 sd = load(g, g->tvec2, g->flood_seed_var);
                u32 seed = c_make(g, df_from_f(g, extract(g, g->tfloat, sd, 0)), df_from_f(g, extract(g, g->tfloat, sd, 1)));
                return sel4(g, is_cont, seed, a0);
            }
            u32 is_start = op2(g, SpvOpIEqual, g->tbool, mode, ci(g, FLOOD_START));
            return sel4(g, is_cont, load(g, g->tvec4, g->flood_from_var), sel4(g, is_start, a0, a1));
        }
        u32 is_cont = op2(g, SpvOpIEqual, g->tbool, mode, ci(g, FLOOD_CONTINUE));
        u32 bc[2] = {is_cont, is_cont};
        u32 bvec = construct(g, spv_type_vec(&g->s, g->tbool, 2), bc, 2);
        if (id == I_FLOOD_SEED) return spv_op3(&g->s, SpvOpSelect, g->tvec2, bvec, load(g, g->tvec2, g->flood_seed_var), a0);
        u32 is_start = op2(g, SpvOpIEqual, g->tbool, mode, ci(g, FLOOD_START));
        u32 bs[2] = {is_start, is_start};
        u32 svec = construct(g, spv_type_vec(&g->s, g->tbool, 2), bs, 2);
        u32 direct = spv_op3(&g->s, SpvOpSelect, g->tvec2, svec, a0, a1);
        return spv_op3(&g->s, SpvOpSelect, g->tvec2, bvec, load(g, g->tvec2, g->flood_from_var), direct);
    }
    if (id == I_APPEND || id == I_CLEAR) {
        Type *lt = e->args[0]->type;
        u32 sc;
        u32 base = lv_ptr(g, e->args[0], &sc);
        u32 z = ci(g, 0);
        u32 lenp = g_access(g, ptr(g, sc, g->tint), base, &z, 1);
        if (id == I_CLEAR) {
            store(g, lenp, ci(g, 0));
            return 0;
        }
        u32 v = gen_expr(g, e->args[1]);
        u32 len = load(g, g->tint, lenp);
        u32 room = op2(g, SpvOpSLessThan, g->tbool, len, ci(g, lt->n));
        u32 yes = new_label(g), merge = new_label(g);
        spv_selection_merge(&g->s, merge);
        spv_branch_cond(&g->s, room, yes, merge);
        label(g, yes);
        u32 idx[2] = {ci(g, 1), len};
        store(g, g_access(g, ptr(g, sc, ty(g, lt->elem)), base, idx, 2), v);
        store(g, lenp, op2(g, SpvOpIAdd, g->tint, len, ci(g, 1)));
        branch(g, merge);
        label(g, merge);
        return 0;
    }
    if (id == I_LENGTH) {
        Type *t = e->args[0]->type;
        if (t->kind == T_VEC) return ci(g, t->n);
        return extract(g, g->tint, gen_expr(g, e->args[0]), 0);
    }
    if (id == I_DOT) {
        Type *t = e->args[0]->type, *el = t->elem;
        u32 a = gen_expr(g, e->args[0]), b = gen_expr(g, e->args[1]);
        u32 acc = cnull(g, el);
        for (int i = 0; i < t->n; i++) {
            u32 x = extract(g, ty(g, el), a, (u32)i), y = extract(g, ty(g, el), b, (u32)i);
            acc = num_op_g(g, el, TK_PLUS, acc, num_op_g(g, el, TK_STAR, x, y));
        }
        return acc;
    }
    u32 a[4] = {0};
    for (int i = 0; i < e->nargs && i < 4; i++) a[i] = gen_expr(g, e->args[i]);
    if (g->df) {
        u32 r = gen_intrinsic_df(g, e, a);
        if (r) return r;
    }
    switch (id) {
    case I_SIN: return ext1(g, f, GLSLSin, a[0]);
    case I_COS: return ext1(g, f, GLSLCos, a[0]);
    case I_TAN: return ext1(g, f, GLSLTan, a[0]);
    case I_ASIN: return ext1(g, f, GLSLAsin, a[0]);
    case I_ACOS: return ext1(g, f, GLSLAcos, a[0]);
    case I_ATAN: return ext1(g, f, GLSLAtan, a[0]);
    case I_SINH: return ext1(g, f, GLSLSinh, a[0]);
    case I_COSH: return ext1(g, f, GLSLCosh, a[0]);
    case I_TANH: return ext1(g, f, GLSLTanh, a[0]);
    case I_ASINH: return ext1(g, f, GLSLAsinh, a[0]);
    case I_ACOSH: return ext1(g, f, GLSLAcosh, a[0]);
    case I_ATANH: return ext1(g, f, GLSLAtanh, a[0]);
    case I_EXP: return ext1(g, f, GLSLExp, a[0]);
    case I_LOG: return ext1(g, f, GLSLLog, a[0]);
    case I_SQRT: return ext1(g, f, GLSLSqrt, a[0]);
    case I_FLOOR: return ext1(g, f, GLSLFloor, a[0]);
    case I_CEIL: return ext1(g, f, GLSLCeil, a[0]);
    case I_FRACT: return ext1(g, f, GLSLFract, a[0]);
    case I_FABS: return ext1(g, f, GLSLFAbs, a[0]);
    case I_FSIGN: return ext1(g, f, GLSLFSign, a[0]);
    case I_EXP2: return ext1(g, f, GLSLExp2, a[0]);
    case I_LOG2: return ext1(g, f, GLSLLog2, a[0]);
    case I_TRUNC: return ext1(g, f, GLSLTrunc, a[0]);
    case I_ATAN2: return ext2(g, f, GLSLAtan2, a[0], a[1]);
    case I_POW: return ext2(g, f, GLSLPow, a[0], a[1]);
    case I_FMIN: return ext2(g, f, GLSLFMin, a[0], a[1]);
    case I_FMAX: return ext2(g, f, GLSLFMax, a[0], a[1]);
    case I_FMOD: return op2(g, SpvOpFMod, f, a[0], a[1]);
    case I_STEP: return ext2(g, f, GLSLStep, a[0], a[1]);
    case I_FCLAMP: return ext3(g, f, GLSLFClamp, a[0], a[1], a[2]);
    case I_FMIX: return ext3(g, f, GLSLFMix, a[0], a[1], a[2]);
    case I_SMOOTHSTEP: return ext3(g, f, GLSLSmoothStep, a[0], a[1], a[2]);
    case I_IABS: return ext1(g, g->tint, GLSLSAbs, a[0]);
    case I_ISIGN: return ext1(g, g->tint, GLSLSSign, a[0]);
    case I_IMIN: return ext2(g, g->tint, GLSLSMin, a[0], a[1]);
    case I_IMAX: return ext2(g, g->tint, GLSLSMax, a[0], a[1]);
    case I_IMOD: return num_op_g(g, t_int, TK_MOD, a[0], a[1]);
    case I_IDIV: {
        u32 t = g->tint;
        u32 z = op2(g, SpvOpIEqual, g->tbool, a[1], ci(g, 0));
        u32 sb = spv_op3(&g->s, SpvOpSelect, t, z, ci(g, 1), a[1]);
        u32 q = op2(g, SpvOpSDiv, t, a[0], sb);
        u32 m = op2(g, SpvOpISub, t, a[0], op2(g, SpvOpIMul, t, q, sb));
        u32 mnz = op2(g, SpvOpINotEqual, g->tbool, m, ci(g, 0));
        u32 sm = op2(g, SpvOpSLessThan, g->tbool, m, ci(g, 0)), sbn = op2(g, SpvOpSLessThan, g->tbool, sb, ci(g, 0));
        u32 adj = op2(g, SpvOpLogicalAnd, g->tbool, mnz, op2(g, SpvOpLogicalNotEqual, g->tbool, sm, sbn));
        u32 q2 = spv_op3(&g->s, SpvOpSelect, t, adj, op2(g, SpvOpISub, t, q, ci(g, 1)), q);
        return spv_op3(&g->s, SpvOpSelect, t, z, ci(g, 0), q2);
    }
    case I_FLOORI: return f2i_clamped(g, ext1(g, f, GLSLFloor, a[0]));
    case I_CEILI: return f2i_clamped(g, ext1(g, f, GLSLCeil, a[0]));
    case I_ROUNDI: return f2i_clamped(g, ext1(g, f, GLSLFloor, op2(g, SpvOpFAdd, f, a[0], cf(g, 0.5f))));
    case I_TRUNCI: return f2i_clamped(g, a[0]);
    case I_COMPLEX: return vec2(g, a[0], a[1]);
    case I_RE: return extract(g, f, a[0], 0);
    case I_IM: return extract(g, f, a[0], 1);
    case I_QUAT: return construct(g, g->tvec4, a, 4);
    case I_PMAKE: {
        u32 p[4] = {extract(g, f, a[0], 0), extract(g, f, a[0], 1), extract(g, f, a[1], 0), extract(g, f, a[1], 1)};
        return proj_scale(g, construct(g, g->tvec4, p, 4));
    }
    case I_PA: return vec2(g, extract(g, f, a[0], 0), extract(g, f, a[0], 1));
    case I_PB: return vec2(g, extract(g, f, a[0], 2), extract(g, f, a[0], 3));
    case I_ISNAN: return op1(g, SpvOpIsNan, g->tbool, a[0]);
    case I_ISINF: return op1(g, SpvOpIsInf, g->tbool, a[0]);
    }
    return cf(g, 0);
}

static u32 gen_call(Gen *g, Expr *e) {
    if (e->intrinsic) return gen_intrinsic(g, e);
    Func *f = e->fn;
    queue_func(g, f);
    u32 o[34];
    o[0] = f->spv_fn;
    for (int i = 0; i < e->nargs && i < 32; i++) o[1 + i] = gen_expr(g, e->args[i]);
    return spv_op(&g->s, SpvOpFunctionCall, ty(g, f->ret), o, 1 + e->nargs);
}

/* ---- expressions --------------------------------------------------------------- */
static bool has_call(Expr *e) {
    if (!e) return false;
    if (e->kind == E_CALL && !e->intrinsic) return true;
    if (has_call(e->a) || has_call(e->b)) return true;
    for (int i = 0; i < e->nargs; i++)
        if (has_call(e->args[i])) return true;
    return false;
}

static u32 gen_binary(Gen *g, Expr *e) {
    int op = e->op;
    if (op == TK_AND || op == TK_OR) {
        u32 a = gen_expr(g, e->a);
        if (!has_call(e->b)) return op2(g, op == TK_AND ? SpvOpLogicalAnd : SpvOpLogicalOr, g->tbool, a, gen_expr(g, e->b));
        u32 tmp = local_var(g, t_bool);
        store(g, tmp, a);
        u32 rhs = new_label(g), merge = new_label(g);
        spv_selection_merge(&g->s, merge);
        if (op == TK_AND) spv_branch_cond(&g->s, a, rhs, merge);
        else spv_branch_cond(&g->s, a, merge, rhs);
        label(g, rhs);
        store(g, tmp, gen_expr(g, e->b));
        branch(g, merge);
        label(g, merge);
        return load(g, g->tbool, tmp);
    }
    Type *ta = e->a->type, *tb = e->b->type;
    u32 a = gen_expr(g, e->a), b = gen_expr(g, e->b);
    if (op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE) {
        static const u32 iops[4] = {SpvOpSLessThan, SpvOpSLessThanEqual, SpvOpSGreaterThan, SpvOpSGreaterThanEqual};
        static const u32 fops[4] = {SpvOpFOrdLessThan, SpvOpFOrdLessThanEqual, SpvOpFOrdGreaterThan, SpvOpFOrdGreaterThanEqual};
        int k = op == TK_LT ? 0 : op == TK_LE ? 1 : op == TK_GT ? 2 : 3;
        if (g->df && ta->kind == T_REAL) return df_cmp(g, op, a, b);
        return op2(g, ta->kind == T_INT ? iops[k] : fops[k], g->tbool, a, b);
    }
    if (op == TK_EQ || op == TK_NE) {
        u32 eq;
        if (ta->kind == T_BOOL) eq = op2(g, SpvOpLogicalEqual, g->tbool, a, b);
        else if (ta->kind == T_INT) eq = op2(g, SpvOpIEqual, g->tbool, a, b);
        else if (ta->kind == T_VEC) {
            u32 parts[64];
            for (int i = 0; i < ta->n; i++)
                parts[i] = num_op_g(g, ta->elem, TK_MINUS, extract(g, ty(g, ta->elem), a, (u32)i), extract(g, ty(g, ta->elem), b, (u32)i));
            eq = approx_zero(g, norm2_g(g, ta, construct(g, ty(g, ta), parts, ta->n)));
        } else {
            eq = approx_zero(g, norm2_g(g, ta, num_op_g(g, ta, TK_MINUS, a, b)));
        }
        return op == TK_EQ ? eq : op1(g, SpvOpLogicalNot, g->tbool, eq);
    }
    if (ta->kind == T_VEC || tb->kind == T_VEC) {
        Type *rt = e->type, *el = rt->elem;
        u32 parts[64], et = ty(g, el);
        for (int i = 0; i < rt->n; i++) {
            u32 x = ta->kind == T_VEC ? extract(g, et, a, (u32)i) : a;
            u32 y = tb->kind == T_VEC ? extract(g, et, b, (u32)i) : b;
            parts[i] = num_op_g(g, el, op, x, y);
        }
        return construct(g, ty(g, rt), parts, rt->n);
    }
    return num_op_g(g, ta, op, a, b);
}

static u32 gen_powi(Gen *g, Type *t, u32 base, int n) {
    if (n == 0) return one_of(g, t);
    u32 m = n < 0 ? (u32)(-(i64)n) : (u32)n;
    u32 acc = 0, b = base;
    while (m) {
        if (m & 1) acc = acc ? num_op_g(g, t, TK_STAR, acc, b) : b;
        m >>= 1;
        if (m) b = num_op_g(g, t, TK_STAR, b, b);
    }
    if (n < 0 && t->kind != T_INT) acc = num_op_g(g, t, TK_SLASH, one_of(g, t), acc);
    return acc;
}

static u32 gen_expr(Gen *g, Expr *e) {
    switch (e->kind) {
    case E_INT: return ci(g, e->ival);
    case E_REAL: return real_const(g, e->rval);
    case E_IMAG: {
        if (g->df) return c_make(g, df_const(g, 0), df_const(g, e->rval));
        float v[2] = {0, (float)e->rval};
        return const_value(g, t_complex, v);
    }
    case E_BOOL: return spv_const_bool(&g->s, e->ival != 0);
    case E_NAME: case E_INTERNAL: case E_MEMBER: {
        Sym *s = e->sym;
        if (s->kind == SYM_CONST) return const_value(g, s->type, s->cval);
        return load(g, ty(g, s->type), s->spv_var);
    }
    case E_CONVERT: {
        Type *from = e->a->type, *to = e->type;
        if (from->kind == T_VEC && to->kind == T_LIST) {
            u32 v = gen_expr(g, e->a);
            u32 items[256];
            int n = FS_MIN(from->n, 256);
            for (int i = 0; i < n; i++) items[i] = convert_g(g, from->elem, to->elem, extract(g, ty(g, from->elem), v, (u32)i));
            return make_list(g, to, items, n);
        }
        return convert_g(g, from, to, gen_expr(g, e->a));
    }
    case E_UNARY: {
        u32 a = gen_expr(g, e->a);
        if (e->op == TK_NOT) return op1(g, SpvOpLogicalNot, g->tbool, a);
        return negate(g, e->type, a);
    }
    case E_BINARY: return gen_binary(g, e);
    case E_POWI: return gen_powi(g, e->type, gen_expr(g, e->a), e->ival);
    case E_ABS: {
        Type *t = e->a->type;
        u32 a = gen_expr(g, e->a);
        if (t->kind == T_INT) return ext1(g, g->tint, GLSLSAbs, a);
        if (t->kind == T_LIST) return extract(g, g->tint, a, 0);
        if (g->df) return t->kind == T_REAL ? df_abs(g, a) : df_sqrt(g, norm2_g(g, t, a));
        if (t->kind == T_REAL) return ext1(g, g->tfloat, GLSLFAbs, a);
        return ext1(g, g->tfloat, GLSLSqrt, norm2_g(g, t, a));
    }
    case E_PRED: {
        u32 n2 = norm2_g(g, e->a->type, gen_expr(g, e->a));
        if (e->op == TK_ESCAPES) {
            u32 r = global_value(g, g->prog->s_escape);
            if (g->df) {
                n2 = df_hi(g, n2);
                r = df_hi(g, r);
            }
            return op2(g, SpvOpFOrdGreaterThan, g->tbool, n2, op2(g, SpvOpFMul, g->tfloat, r, r));
        }
        return approx_zero(g, n2);
    }
    case E_CALL: return gen_call(g, e);
    case E_INDEX: return gen_index(g, e);
    case E_VECTOR: {
        Type *t = e->type;
        u32 items[256];
        int n = FS_MIN(e->nargs, 256);
        for (int i = 0; i < n; i++) items[i] = gen_expr(g, e->args[i]);
        if (t->kind == T_LIST) return make_list(g, t, items, n);
        return construct(g, ty(g, t), items, n);
    }
    case E_TUPLE: {
        u32 items[64];
        for (int i = 0; i < e->nargs && i < 64; i++) items[i] = gen_expr(g, e->args[i]);
        return construct(g, ty(g, e->type), items, FS_MIN(e->nargs, 64));
    }
    case E_COLOR_CONST: return const_value(g, e->type, e->cval);
    case E_STRING: case E_SOLVE: return 0;
    case E_COND: { /* branches, so that only one value is computed */
        u32 c = gen_expr(g, e->args[0]);
        u32 tmp = local_var(g, e->type);
        u32 lt = new_label(g), lf = new_label(g), merge = new_label(g);
        spv_selection_merge(&g->s, merge);
        spv_branch_cond(&g->s, c, lt, lf);
        label(g, lt);
        store(g, tmp, gen_expr(g, e->args[1]));
        branch(g, merge);
        label(g, lf);
        store(g, tmp, gen_expr(g, e->args[2]));
        branch(g, merge);
        label(g, merge);
        return load(g, ty(g, e->type), tmp);
    }
    }
    return 0;
}

/* ---- statements -------------------------------------------------------------- */
/* component i of the colour, as a float */
static u32 color_part(Gen *g, u32 col, int i) {
    if (g->df) return df_hi(g, extract(g, g->tvec2, col, (u32)i));
    return extract(g, g->tfloat, col, (u32)i);
}

static void write_output(Gen *g) {
    Section *v = g->viewer;
    u32 col = load(g, ty(g, t_color), v->color->spv_var);
    u32 p[4];
    for (int i = 0; i < 3; i++)
        p[i] = ext3(g, g->tfloat, GLSLFClamp, color_part(g, col, i), cf(g, 0), cf(g, 1));
    p[3] = cf(g, 1);
    store(g, g->out_color, construct(g, g->tvec4, p, 4));
}

/* compute variant: colour into the colour buffer, solution and status into the field */
static void write_output_compute(Gen *g) {
    Section *v = g->viewer;
    u32 col = load(g, ty(g, t_color), v->color->spv_var);
    u32 packed = spv_const_u32(&g->s, g->tuint, 0xff000000u);
    for (int i = 0; i < 3; i++) {
        u32 c = ext3(g, g->tfloat, GLSLFClamp, color_part(g, col, i), cf(g, 0), cf(g, 1));
        u32 b = op1(g, SpvOpConvertFToU, g->tuint, op2(g, SpvOpFAdd, g->tfloat, op2(g, SpvOpFMul, g->tfloat, c, cf(g, 255.0f)), cf(g, 0.5f)));
        if (i) b = op2(g, SpvOpShiftLeftLogical, g->tuint, b, spv_const_u32(&g->s, g->tuint, (u32)(8 * i)));
        packed = op2(g, SpvOpBitwiseOr, g->tuint, packed, b);
    }
    u32 idx = load(g, g->tint, g->pixel_var);
    u32 zero = ci(g, 0), one = ci(g, 1);
    u32 ok = load(g, g->tint, g->flood_out_ok_var);
    /* mending: a pixel whose new solve fails keeps what it had */
    return_if(g, op2(g, SpvOpLogicalAnd, g->tbool,
                     op2(g, SpvOpIEqual, g->tbool, load(g, g->tint, g->flood_pass_var), ci(g, FLOOD_PASS_MEND)),
                     op2(g, SpvOpINotEqual, g->tbool, ok, one)));
    u32 ci_[2] = {zero, idx};
    store(g, g_access(g, ptr(g, SpvSCUniform, g->tuint), g->colors_var, ci_, 2), packed);
    u32 status = spv_op3(&g->s, SpvOpSelect, g->tuint, op2(g, SpvOpIEqual, g->tbool, ok, one),
                         spv_const_u32(&g->s, g->tuint, 1 /* FLOOD_DONE */), spv_const_u32(&g->s, g->tuint, 2 /* FLOOD_FAILED */));
    u32 ti[3] = {zero, idx, zero};
    store(g, g_access(g, ptr(g, SpvSCUniform, g->tvec2), g->field_var, ti, 3), load(g, g->tvec2, g->flood_out_t_var));
    u32 si[3] = {zero, idx, ci(g, 1)};
    store(g, g_access(g, ptr(g, SpvSCUniform, g->tuint), g->field_var, si, 3), status);
}

static void gen_return(Gen *g) {
    if (!g->fn && g->compute) {
        write_output_compute(g);
        spv_void(&g->s, SpvOpReturn, NULL, 0);
    } else if (!g->fn) {
        write_output(g);
        spv_void(&g->s, SpvOpReturn, NULL, 0);
    } else if (g->fn->noutputs == 0) { /* a function without a result */
        spv_void(&g->s, SpvOpReturn, NULL, 0);
    } else {
        Func *f = g->fn;
        u32 v;
        if (f->noutputs == 1) {
            v = load(g, ty(g, f->outputs[0]->type), f->outputs[0]->spv_var);
        } else {
            u32 items[64];
            for (int i = 0; i < f->noutputs && i < 64; i++) items[i] = load(g, ty(g, f->outputs[i]->type), f->outputs[i]->spv_var);
            v = construct(g, ty(g, f->ret), items, FS_MIN(f->noutputs, 64));
        }
        spv_void(&g->s, SpvOpReturnValue, &v, 1);
    }
    g->dead = true;
}

static void store_to(Gen *g, Expr *target, u32 v) {
    if (target->sym && target->sym->kind == SYM_EXEC) return;
    u32 sc;
    if (g->df && target->kind == E_INDEX && target->a->type->kind == T_COMPLEX) { /* z[k] = v: rebuild z */
        u32 base = lv_ptr(g, target->a, &sc);
        u32 k = gen_expr(g, target->b);
        u32 z = load(g, g->tvec4, base);
        u32 second = op2(g, SpvOpIEqual, g->tbool, g_clamp_index(g, k, ci(g, 1), ci(g, 2)), ci(g, 1));
        store(g, base, sel4(g, second, c_make(g, c_re(g, z), v), c_make(g, v, c_im(g, z))));
        return;
    }
    if (g->df && target->kind == E_INDEX && target->a->type->kind == T_QUAT) v = df_hi(g, v); /* q[k] = v */
    u32 p = lv_ptr(g, target, &sc);
    store(g, p, v);
}

typedef struct LoopVars { u32 cnt, stk, header, cont, merge; bool quiet, range; } LoopVars;

/* quiet loops (generated by autodiff.c) have no limit and leave .stuck alone; `range` loops (for) run
   their whole range, up to FS_FOR_LIMIT steps; the others stop after .maxIterations */
static LoopVars loop_begin(Gen *g, bool quiet, bool range) {
    LoopVars L;
    L.quiet = quiet;
    L.range = range;
    L.cnt = local_var_id(g, g->tint);
    L.stk = local_var_id(g, g->tbool);
    store(g, L.cnt, ci(g, 0));
    store(g, L.stk, spv_const_bool(&g->s, false));
    L.header = new_label(g);
    L.cont = new_label(g);
    L.merge = new_label(g);
    branch(g, L.header);
    label(g, L.header);
    spv_loop_merge(&g->s, L.merge, L.cont);
    return L;
}

/* evaluate: ok = cnt < max, stuck = !ok && c, returns ok && c */
static u32 loop_check(Gen *g, LoopVars *L, u32 n, u32 c) {
    u32 maxit = L->quiet ? ci(g, 0x7fffffff) : L->range ? ci(g, FS_FOR_LIMIT) : global_value(g, g->prog->s_max_iter);
    u32 ok = op2(g, SpvOpSLessThan, g->tbool, n, maxit);
    u32 st = op2(g, SpvOpLogicalAnd, g->tbool, op1(g, SpvOpLogicalNot, g->tbool, ok), c);
    store(g, L->stk, st);
    return op2(g, SpvOpLogicalAnd, g->tbool, ok, c);
}

static void loop_body(Gen *g, LoopVars *L, Block *b) {
    if (g->nloops < 64) {
        g->loops[g->nloops].merge = L->merge;
        g->loops[g->nloops].cont = L->cont;
    }
    g->nloops++;
    gen_block(g, b);
    g->nloops--;
    if (!g->dead) branch(g, L->cont);
}

static void loop_end(Gen *g, LoopVars *L) {
    label(g, L->merge);
    if (L->quiet || (g->fn && g->fn->prelude)) return; /* library loops don't touch .stuck/.iterations */
    store(g, g->iter_var, load(g, g->tint, L->cnt));
    store(g, g->stuck_var, load(g, g->tbool, L->stk));
}

static void gen_stmt(Gen *g, Stmt *s) {
    switch (s->kind) {
    case S_DEFINE: case S_APPEND: case S_DRAW: case S_ON: case S_ERASE: case S_PRINT: break;
    case S_EXPR:
        if (s->value) gen_expr(g, s->value);
        break;
    case S_DECL: case S_ASSIGN: {
        if (!s->target->sym && s->target->kind != E_INDEX) break;
        u32 v = s->value ? gen_expr(g, s->value) : cnull(g, s->target->type);
        if (s->internal && s->target->sym && s->target->sym->kind == SYM_EXEC) { /* generated code sets .stuck */
            store(g, s->target->sym->spv_var, v);
            break;
        }
        store_to(g, s->target, v);
        break;
    }
    case S_DESTRUCTURE: {
        Type *tt = s->value->type;
        u32 v = gen_expr(g, s->value);
        for (int i = 0; i < s->ntargets; i++) {
            Expr *tg = s->targets[i];
            u32 item = extract(g, ty(g, tt->items[i]), v, (u32)i);
            store_to(g, tg, convert_g(g, tt->items[i], tg->type, item));
        }
        break;
    }
    case S_IF: {
        u32 c = gen_expr(g, s->value);
        u32 then = new_label(g), els = s->else_body.n ? new_label(g) : 0, merge = new_label(g);
        spv_selection_merge(&g->s, merge);
        spv_branch_cond(&g->s, c, then, els ? els : merge);
        label(g, then);
        gen_block(g, &s->body);
        bool then_dead = g->dead;
        if (!g->dead) branch(g, merge);
        bool else_dead = false;
        if (els) {
            label(g, els);
            gen_block(g, &s->else_body);
            else_dead = g->dead;
            if (!g->dead) branch(g, merge);
        }
        label(g, merge);
        if (then_dead && else_dead) {
            spv_void(&g->s, SpvOpUnreachable, NULL, 0);
            g->dead = true;
        }
        break;
    }
    case S_WHILE: {
        LoopVars L = loop_begin(g, s->quiet, false);
        u32 check = new_label(g), body = new_label(g);
        branch(g, check);
        label(g, check);
        u32 n = load(g, g->tint, L.cnt);
        u32 c = gen_expr(g, s->value);
        u32 go = loop_check(g, &L, n, c);
        spv_branch_cond(&g->s, go, body, L.merge);
        label(g, body);
        store(g, L.cnt, op2(g, SpvOpIAdd, g->tint, n, ci(g, 1)));
        loop_body(g, &L, &s->body);
        label(g, L.cont);
        branch(g, L.header);
        loop_end(g, &L);
        break;
    }
    case S_DOWHILE: {
        LoopVars L = loop_begin(g, s->quiet, false);
        u32 body = new_label(g);
        branch(g, body);
        label(g, body);
        u32 n0 = load(g, g->tint, L.cnt);
        store(g, L.cnt, op2(g, SpvOpIAdd, g->tint, n0, ci(g, 1)));
        loop_body(g, &L, &s->body);
        label(g, L.cont);
        u32 n = load(g, g->tint, L.cnt);
        u32 c = gen_expr(g, s->value);
        u32 go = loop_check(g, &L, n, c);
        spv_branch_cond(&g->s, go, L.header, L.merge);
        loop_end(g, &L);
        break;
    }
    case S_FOR: {
        u32 lo = gen_expr(g, s->from), hi = gen_expr(g, s->to);
        u32 st = s->step ? gen_expr(g, s->step) : ci(g, 1);
        u32 kv = s->sym->spv_var;
        store(g, kv, lo);
        LoopVars L = loop_begin(g, s->quiet, true);
        u32 check = new_label(g), body = new_label(g);
        branch(g, check);
        label(g, check);
        u32 n = load(g, g->tint, L.cnt);
        u32 k = load(g, g->tint, kv);
        u32 pos = op2(g, SpvOpSGreaterThanEqual, g->tbool, st, ci(g, 0));
        u32 le = op2(g, SpvOpSLessThanEqual, g->tbool, k, hi), ge = op2(g, SpvOpSGreaterThanEqual, g->tbool, k, hi);
        u32 c = spv_op3(&g->s, SpvOpSelect, g->tbool, pos, le, ge);
        u32 go = loop_check(g, &L, n, c);
        spv_branch_cond(&g->s, go, body, L.merge);
        label(g, body);
        store(g, L.cnt, op2(g, SpvOpIAdd, g->tint, n, ci(g, 1)));
        loop_body(g, &L, &s->body);
        label(g, L.cont);
        store(g, kv, op2(g, SpvOpIAdd, g->tint, load(g, g->tint, kv), st));
        branch(g, L.header);
        loop_end(g, &L);
        break;
    }
    case S_FOREACH: {
        Type *ct = s->from->type;
        u32 coll = local_var(g, ct);
        store(g, coll, gen_expr(g, s->from));
        u32 len;
        if (ct->kind == T_VEC) {
            len = ci(g, ct->n);
        } else {
            u32 z = ci(g, 0);
            len = load(g, g->tint, g_access(g, ptr(g, SpvSCFunction, g->tint), coll, &z, 1));
        }
        u32 jv = local_var_id(g, g->tint);
        store(g, jv, ci(g, 0));
        LoopVars L = loop_begin(g, s->quiet, true);
        u32 check = new_label(g), body = new_label(g);
        branch(g, check);
        label(g, check);
        u32 n = load(g, g->tint, L.cnt);
        u32 j = load(g, g->tint, jv);
        u32 c = op2(g, SpvOpSLessThan, g->tbool, j, len);
        u32 go = loop_check(g, &L, n, c);
        spv_branch_cond(&g->s, go, body, L.merge);
        label(g, body);
        store(g, L.cnt, op2(g, SpvOpIAdd, g->tint, n, ci(g, 1)));
        u32 et = ty(g, ct->elem);
        u32 ep;
        if (ct->kind == T_VEC) {
            ep = g_access(g, ptr(g, SpvSCFunction, et), coll, &j, 1);
        } else {
            u32 idx[2] = {ci(g, 1), j};
            ep = g_access(g, ptr(g, SpvSCFunction, et), coll, idx, 2);
        }
        store(g, s->sym->spv_var, load(g, et, ep));
        store(g, jv, op2(g, SpvOpIAdd, g->tint, j, ci(g, 1)));
        loop_body(g, &L, &s->body);
        label(g, L.cont);
        branch(g, L.header);
        loop_end(g, &L);
        break;
    }
    case S_BREAK:
        if (g->nloops > 0 && g->nloops <= 64) branch(g, g->loops[g->nloops - 1].merge);
        g->dead = true;
        break;
    case S_CONTINUE:
        if (g->nloops > 0 && g->nloops <= 64) branch(g, g->loops[g->nloops - 1].cont);
        g->dead = true;
        break;
    case S_RETURN: gen_return(g); break;
    }
}

static void gen_block(Gen *g, Block *b) {
    for (int i = 0; i < b->n && !g->dead; i++) gen_stmt(g, b->stmts[i]);
}

/* ---- usage analysis ---------------------------------------------------------- */
static void mark_block(Gen *g, Block *b);
static void mark_func(Gen *g, Func *f) {
    for (int i = 0; i < g->visited.len; i++)
        if (g->visited.data[i] == f) return;
    vec_push(g->visited, f);
    if (f->body_expr) {
        mark_expr(g, f->body_expr);
    } else {
        mark_block(g, &f->body);
    }
}
static void mark_expr(Gen *g, Expr *e) {
    if (!e) return;
    if (e->sym) e->sym->used = true;
    if (e->kind == E_CALL && e->fn) mark_func(g, e->fn);
    mark_expr(g, e->a);
    mark_expr(g, e->b);
    for (int i = 0; i < e->nargs; i++) mark_expr(g, e->args[i]);
}
static void mark_block(Gen *g, Block *b) {
    for (int i = 0; i < b->n; i++) {
        Stmt *s = b->stmts[i];
        mark_expr(g, s->target);
        mark_expr(g, s->value);
        mark_expr(g, s->from);
        mark_expr(g, s->to);
        mark_expr(g, s->step);
        for (int k = 0; k < s->ntargets; k++) mark_expr(g, s->targets[k]);
        mark_block(g, &s->body);
        mark_block(g, &s->else_body);
    }
}

/* Debug names (OpName).  Translators turn them into identifiers: SPIRV-Cross
 * keeps a function called "clamp" or "atan2" as is, which then clashes with
 * the Metal library functions of the same name (MoltenVK fails to compile the
 * shader).  So every name gets a prefix and is reduced to [A-Za-z0-9_]. */
static void spv_name_safe(Spv *s, u32 id, const char *prefix, const char *name) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%s", prefix);
    for (const u8 *c = (const u8 *)name; *c && n < (int)sizeof buf - 1; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        buf[n++] = ok ? (char)*c : '_';
    }
    buf[n] = 0;
    spv_name(s, id, buf);
}

/* ---- functions and module ------------------------------------------------------ */
static void emit_func(Gen *g, Func *f) {
    u32 ptypes[32], pids[32];
    int np = FS_MIN(f->nparams, 32);
    for (int i = 0; i < np; i++) ptypes[i] = ty(g, f->params[i]->type);
    u32 ret = ty(g, f->ret);
    u32 ft = spv_type_fn(&g->s, ret, ptypes, np);
    spv_begin_function(&g->s, f->spv_fn, ret, ft, ptypes, pids, np);
    spv_name_safe(&g->s, f->spv_fn, "fs_", f->name);
    for (int i = 0; i < f->locals.len; i++) {
        Sym *s = f->locals.data[i];
        s->spv_var = local_var(g, s->type);
    }
    for (int i = 0; i < np; i++) store(g, f->params[i]->spv_var, pids[i]);
    g->fn = f;
    g->dead = false;
    g->nloops = 0;
    if (f->body_expr) {
        u32 v = gen_expr(g, f->body_expr);
        spv_void(&g->s, SpvOpReturnValue, &v, 1);
    } else {
        gen_block(g, &f->body);
        if (!g->dead) gen_return(g);
    }
    spv_end_function(&g->s);
}

static void reset_ids(Program *p) {
    for (Type *t = type_intern_list; t; t = t->next) t->spv_id = t->spv_buf_id = t->spv_buf_aux = 0;
    for (int i = 0; i < p->funcs.len; i++) {
        p->funcs.data[i]->spv_fn = 0;
        for (int k = 0; k < p->funcs.data[i]->locals.len; k++) p->funcs.data[i]->locals.data[k]->spv_var = 0;
    }
    for (int i = 0; i < p->globals.len; i++) {
        p->globals.data[i]->spv_var = 0;
        p->globals.data[i]->used = false;
    }
    p->s_stuck->spv_var = p->s_iterations->spv_var = 0;
}

/* buffer value (bty) -> logical value (ty) */
static u32 from_buffer(Gen *g, Type *t, u32 v) {
    switch (t->kind) {
    case T_BOOL: return op2(g, SpvOpINotEqual, g->tbool, v, spv_const_u32(&g->s, g->tuint, 0));
    case T_REAL: return g->df ? df_from_f(g, v) : v;
    case T_COMPLEX:
        return g->df ? c_make(g, df_from_f(g, extract(g, g->tfloat, v, 0)), df_from_f(g, extract(g, g->tfloat, v, 1))) : v;
    case T_VEC: {
        u32 parts[64];
        for (int i = 0; i < t->n; i++) parts[i] = from_buffer(g, t->elem, extract(g, bty(g, t->elem), v, (u32)i));
        return construct(g, ty(g, t), parts, t->n);
    }
    case T_LIST: {
        u32 len = extract(g, g->tint, v, 0);
        u32 arr = extract(g, t->spv_buf_aux, v, 1);
        u32 *parts = (u32 *)xmalloc(sizeof(u32) * (size_t)t->n);
        for (int i = 0; i < t->n; i++) parts[i] = from_buffer(g, t->elem, extract(g, bty(g, t->elem), arr, (u32)i));
        u32 la = construct(g, ty(g, type_vec(t->elem, t->n)), parts, t->n);
        free(parts);
        u32 m[2] = {len, la};
        return construct(g, ty(g, t), m, 2);
    }
    default: return v;
    }
}

/* Which globals does viewer `v` read (directly or through functions)?  used[i] for p->globals.data[i]. */
void codegen_viewer_deps(Program *p, Section *v, bool *used) {
    Gen G = {0};
    G.prog = p;
    for (int i = 0; i < p->globals.len; i++) p->globals.data[i]->used = false;
    mark_block(&G, &v->body);
    v->center->used = v->radius->used = true;
    if (v->mobius) v->mobius->used = true;
    for (int i = 0; i < p->globals.len; i++) used[i] = p->globals.data[i]->used;
    vec_free(G.visited);
}

/* Generate the fragment shader of viewer `v`.  Returns malloc'd words or NULL. */
/* if `bad` then return (structured: the return block sits inside a selection) */
static void return_if(Gen *g, u32 bad) {
    u32 ret = new_label(g), go = new_label(g);
    spv_selection_merge(&g->s, go);
    spv_branch_cond(&g->s, bad, ret, go);
    label(g, ret);
    spv_void(&g->s, SpvOpReturn, NULL, 0);
    label(g, go);
}

/* the view's radius as a float */
static u32 radius_f(Gen *g) {
    u32 r = global_value(g, g->viewer->radius);
    return g->df ? df_hi(g, r) : r;
}

/* plane point of the pixel position (px, py) in a w x h flat (non-projective) view */
static u32 flat_plane_point(Gen *g, u32 px, u32 py, u32 w, u32 h) {
    Section *v = g->viewer;
    u32 f = g->tfloat;
    u32 half = cf(g, 0.5f);
    u32 m = op2(g, SpvOpFMul, f, half, ext2(g, f, GLSLFMin, w, h));
    u32 scale = op2(g, SpvOpFDiv, f, radius_f(g), m);
    u32 dx = op2(g, SpvOpFSub, f, px, op2(g, SpvOpFMul, f, half, w));
    u32 dy = op2(g, SpvOpFSub, f, op2(g, SpvOpFMul, f, half, h), py);
    u32 c = global_value(g, v->center);
    if (g->df) { /* centre (hi + lo, kept by the app) + the offset, in double-single precision */
        u32 lo = v->center_lo ? load(g, g->tvec4, v->center_lo->spv_var) : df_const(g, 0);
        u32 cre = v->center_lo ? df_add(g, c_re(g, c), c_re(g, lo)) : c_re(g, c);
        u32 cim = v->center_lo ? df_add(g, c_im(g, c), c_im(g, lo)) : c_im(g, c);
        return c_make(g, df_add_f(g, cre, op2(g, SpvOpFMul, f, scale, dx)), df_add_f(g, cim, op2(g, SpvOpFMul, f, scale, dy)));
    }
    u32 re = op2(g, SpvOpFAdd, f, extract(g, f, c, 0), op2(g, SpvOpFMul, f, scale, dx));
    u32 im = op2(g, SpvOpFAdd, f, extract(g, f, c, 1), op2(g, SpvOpFMul, f, scale, dy));
    return vec2(g, re, im);
}

/* size of a pixel of a w x h flat view, in plane units (a value of type R) */
static u32 flat_pixel_size(Gen *g, u32 w, u32 h) {
    u32 f = g->tfloat;
    u32 m = op2(g, SpvOpFMul, f, cf(g, 0.5f), ext2(g, f, GLSLFMin, w, h));
    u32 px = op2(g, SpvOpFDiv, f, radius_f(g), m);
    return g->df ? df_from_f(g, px) : px;
}

/* a lift (num, den) (vec4) of the point of the pixel position (px, py) in a w x h projective view (see
   proj_pixel_to_plane in interp.c) */
static u32 proj_plane_lift(Gen *g, u32 px, u32 py, u32 w, u32 h);

/* plane point of the pixel position (px, py) in a w x h projective view */
static u32 proj_plane_point(Gen *g, u32 px, u32 py, u32 w, u32 h) {
    u32 l = proj_plane_lift(g, px, py, w, h), f = g->tfloat;
    return g_cdiv(g, vec2(g, extract(g, f, l, 0), extract(g, f, l, 1)), vec2(g, extract(g, f, l, 2), extract(g, f, l, 3)));
}

static u32 proj_plane_lift(Gen *g, u32 px, u32 py, u32 w, u32 h) {
    Section *v = g->viewer;
    u32 f = g->tfloat;
    u32 half = cf(g, 0.5f);
    u32 m = op2(g, SpvOpFMul, f, half, ext2(g, f, GLSLFMin, w, h));
    u32 dx = op2(g, SpvOpFSub, f, px, op2(g, SpvOpFMul, f, half, w));
    u32 dy = op2(g, SpvOpFSub, f, op2(g, SpvOpFMul, f, half, h), py);
    u32 T = cf(g, 1.19175359f);
    u32 x = op2(g, SpvOpFMul, f, op2(g, SpvOpFDiv, f, dx, m), T);
    u32 y = op2(g, SpvOpFMul, f, op2(g, SpvOpFDiv, f, dy, m), T);
    u32 r2 = op2(g, SpvOpFAdd, f, op2(g, SpvOpFAdd, f, op2(g, SpvOpFMul, f, x, x), op2(g, SpvOpFMul, f, y, y)), cf(g, 1));
    u32 n = op2(g, SpvOpFDiv, f, cf(g, 1), ext1(g, f, GLSLSqrt, r2));
    u32 u = vec2(g, op2(g, SpvOpFMul, f, x, n), op2(g, SpvOpFMul, f, y, n));
    u32 vv = vec2(g, op2(g, SpvOpFAdd, f, cf(g, 1), n), cf(g, 0));
    u32 mv = global_value(g, v->mobius);
    u32 ma = extract(g, g->tvec2, mv, 0), mb = extract(g, g->tvec2, mv, 1);
    u32 mc = extract(g, g->tvec2, mv, 2), md = extract(g, g->tvec2, mv, 3);
    u32 num = op2(g, SpvOpFAdd, g->tvec2, g_cmul(g, ma, u), g_cmul(g, mb, vv));
    u32 den = op2(g, SpvOpFAdd, g->tvec2, g_cmul(g, mc, u), g_cmul(g, md, vv));
    u32 p[4] = {extract(g, f, num, 0), extract(g, f, num, 1), extract(g, f, den, 0), extract(g, f, den, 1)};
    return construct(g, g->tvec4, p, 4);
}

static u32 iabs_(Gen *g, u32 x) { return ext1(g, g->tint, GLSLSAbs, x); }
static u32 imax_(Gen *g, u32 a, u32 b) { return ext2(g, g->tint, GLSLSMax, a, b); }
static u32 isel(Gen *g, u32 c, u32 a, u32 b) { return spv_op3(&g->s, SpvOpSelect, g->tint, c, a, b); }
static u32 fsel(Gen *g, u32 c, u32 a, u32 b) { return spv_op3(&g->s, SpvOpSelect, g->tfloat, c, a, b); }
static u32 band(Gen *g, u32 a, u32 b) { return op2(g, SpvOpLogicalAnd, g->tbool, a, b); }
static u32 bnot(Gen *g, u32 a) { return op1(g, SpvOpLogicalNot, g->tbool, a); }

/* ---- mending (see flood_mend_source in flood.c, which this must match) ---------------------- */
typedef struct FloodPix { u32 done, t; } FloodPix; /* converged (bool), solution (vec2) */

/* the entry of pixel (x + j ux, y + j uy): converged inside the image, and its solution
 * (ux, uy: ids of ints, (1, 0) or (0, 1)) */
static FloodPix mend_pix(Gen *g, u32 x, u32 y, u32 W, u32 H, u32 j, u32 ux, u32 uy) {
    u32 I = g->tint, B = g->tbool, z0 = ci(g, 0), z1 = ci(g, 1);
    u32 px = op2(g, SpvOpIAdd, I, x, op2(g, SpvOpIMul, I, j, ux)), py = op2(g, SpvOpIAdd, I, y, op2(g, SpvOpIMul, I, j, uy));
    u32 in = band(g, band(g, op2(g, SpvOpSGreaterThanEqual, B, px, z0), op2(g, SpvOpSGreaterThanEqual, B, py, z0)),
                  band(g, op2(g, SpvOpSLessThan, B, px, W), op2(g, SpvOpSLessThan, B, py, H)));
    u32 safe = op2(g, SpvOpIAdd, I, op2(g, SpvOpIMul, I, ext3(g, I, GLSLSClamp, py, z0, op2(g, SpvOpISub, I, H, z1)), W),
                   ext3(g, I, GLSLSClamp, px, z0, op2(g, SpvOpISub, I, W, z1)));
    u32 si[3] = {z0, safe, z1}, ti[3] = {z0, safe, z0};
    u32 st = load(g, g->tuint, g_access(g, ptr(g, SpvSCUniform, g->tuint), g->field_var, si, 3));
    FloodPix r;
    r.done = band(g, in, op2(g, SpvOpIEqual, B, st, spv_const_u32(&g->s, g->tuint, 1 /* FLOOD_DONE */)));
    r.t = load(g, g->tvec2, g_access(g, ptr(g, SpvSCUniform, g->tvec2), g->field_var, ti, 3));
    return r;
}

static u32 mend_abs(Gen *g, u32 t) { /* sqrt(re^2 + im^2), as flood_cabs */
    u32 f = g->tfloat, re = extract(g, f, t, 0), im = extract(g, f, t, 1);
    return ext1(g, f, GLSLSqrt, op2(g, SpvOpFAdd, f, op2(g, SpvOpFMul, f, re, re), op2(g, SpvOpFMul, f, im, im)));
}

static u32 mend_diff(Gen *g, FloodPix a, FloodPix b) { /* |t_b - t_a| or FLOOD_FAR */
    return fsel(g, band(g, a.done, b.done), mend_abs(g, op2(g, SpvOpFSub, g->tvec2, b.t, a.t)), cf(g, 1e30f));
}

static u32 fmin_(Gen *g, u32 a, u32 b) { return fsel(g, op2(g, SpvOpFOrdLessThan, g->tbool, a, b), a, b); }

/* flood_line_jumps: a loop over k = 1 .. kmax collecting the first FLOOD_MEND_JUMPS jumps (0: none).
 * The pixels a = s (k - 1), b = s k and the differences around them slide along the line, so each
 * step loads one pixel (b + s) and computes one difference. */
static void mend_jumps(Gen *g, u32 x, u32 y, u32 W, u32 H, u32 ux, u32 uy, int sgn, u32 kmax, u32 enabled, u32 out[FLOOD_MEND_JUMPS]) {
    u32 I = g->tint, B = g->tbool, F = g->tfloat, V2 = g->tvec2;
    u32 kv[FLOOD_MEND_JUMPS];
    for (int i = 0; i < FLOOD_MEND_JUMPS; i++) {
        kv[i] = local_var_id(g, I);
        store(g, kv[i], ci(g, 0));
    }
    u32 n = local_var_id(g, I), stop = local_var_id(g, B), kk = local_var_id(g, I);
    u32 v_dam = local_var_id(g, F), v_dab = local_var_id(g, F), v_ma = local_var_id(g, F), v_mb = local_var_id(g, F);
    u32 v_bdone = local_var_id(g, B), v_bt = local_var_id(g, V2);
    u32 s = ci(g, sgn);
    {
        FloodPix am = mend_pix(g, x, y, W, H, op1(g, SpvOpSNegate, I, s), ux, uy);
        FloodPix a = mend_pix(g, x, y, W, H, ci(g, 0), ux, uy);
        FloodPix b = mend_pix(g, x, y, W, H, s, ux, uy);
        store(g, v_dam, mend_diff(g, am, a));
        store(g, v_dab, mend_diff(g, a, b));
        store(g, v_ma, mend_abs(g, a.t));
        store(g, v_mb, mend_abs(g, b.t));
        store(g, v_bdone, b.done);
        store(g, v_bt, b.t);
    }
    store(g, n, ci(g, 0));
    store(g, stop, bnot(g, enabled));
    store(g, kk, ci(g, 1));
    u32 header = new_label(g), check = new_label(g), body = new_label(g), cont = new_label(g), merge = new_label(g);
    branch(g, header);
    label(g, header);
    spv_loop_merge(&g->s, merge, cont);
    branch(g, check);
    label(g, check);
    u32 k = load(g, I, kk);
    u32 go = band(g, band(g, op2(g, SpvOpSLessThanEqual, B, k, kmax), op2(g, SpvOpSLessThan, B, load(g, I, n), ci(g, FLOOD_MEND_JUMPS))),
                  bnot(g, load(g, B, stop)));
    spv_branch_cond(&g->s, go, body, merge);
    label(g, body);
    FloodPix b = {load(g, B, v_bdone), load(g, V2, v_bt)};
    FloodPix bp = mend_pix(g, x, y, W, H, op2(g, SpvOpIMul, I, s, op2(g, SpvOpIAdd, I, k, ci(g, 1))), ux, uy);
    u32 vfar = cf(g, 1e30f);
    u32 d = load(g, F, v_dab), dbn = mend_diff(g, b, bp);
    u32 gg = fmin_(g, load(g, F, v_dam), dbn);
    u32 size = op2(g, SpvOpFAdd, F, load(g, F, v_ma), load(g, F, v_mb));
    u32 lim = op2(g, SpvOpFAdd, F, op2(g, SpvOpFMul, F, cf(g, 8.0f), gg), op2(g, SpvOpFMul, F, cf(g, 1e-4f), size));
    u32 jump = band(g, band(g, b.done, op2(g, SpvOpFOrdLessThan, B, d, vfar)),
                    band(g, op2(g, SpvOpFOrdLessThan, B, gg, vfar), op2(g, SpvOpFOrdGreaterThan, B, d, lim)));
    u32 nv = load(g, I, n);
    for (int i = 0; i < FLOOD_MEND_JUMPS; i++)
        store(g, kv[i], isel(g, band(g, jump, op2(g, SpvOpIEqual, B, nv, ci(g, i))), k, load(g, I, kv[i])));
    store(g, n, op2(g, SpvOpIAdd, I, nv, isel(g, jump, ci(g, 1), ci(g, 0))));
    store(g, stop, bnot(g, b.done));
    /* slide: a <- b, b <- b + s */
    store(g, v_dam, d);
    store(g, v_dab, dbn);
    store(g, v_ma, load(g, F, v_mb));
    store(g, v_mb, mend_abs(g, bp.t));
    store(g, v_bdone, bp.done);
    store(g, v_bt, bp.t);
    branch(g, cont);
    label(g, cont);
    store(g, kk, op2(g, SpvOpIAdd, I, k, ci(g, 1)));
    branch(g, header);
    label(g, merge);
    for (int i = 0; i < FLOOD_MEND_JUMPS; i++) out[i] = load(g, I, kv[i]);
}

/* flood_mend_axis: the band's thickness (0: none) and the pixel to continue from */
static u32 mend_axis(Gen *g, u32 x, u32 y, u32 W, u32 H, u32 ux, u32 uy, u32 kmax, u32 *src) {
    u32 I = g->tint, B = g->tbool, F = g->tfloat, z0 = ci(g, 0), z1 = ci(g, 1);
    enum { NJ = FLOOD_MEND_JUMPS };
    u32 kp[NJ], km[NJ];
    mend_jumps(g, x, y, W, H, ux, uy, 1, kmax, spv_const_bool(&g->s, true), kp);
    mend_jumps(g, x, y, W, H, ux, uy, -1, kmax, op2(g, SpvOpSGreaterThan, B, kp[0], z0), km); /* no jump on one side: no band */
    /* the pixels beyond the jumps, the next ones outwards, and the ones before the jumps */
    FloodPix op[NJ], opo[NJ], opi[NJ], om[NJ], omo[NJ], omi[NJ];
    u32 nkm[NJ];
    for (int i = 0; i < NJ; i++) {
        nkm[i] = op1(g, SpvOpSNegate, I, km[i]);
        op[i] = mend_pix(g, x, y, W, H, kp[i], ux, uy);
        opo[i] = mend_pix(g, x, y, W, H, op2(g, SpvOpIAdd, I, kp[i], z1), ux, uy);
        opi[i] = mend_pix(g, x, y, W, H, op2(g, SpvOpISub, I, kp[i], z1), ux, uy);
        om[i] = mend_pix(g, x, y, W, H, nkm[i], ux, uy);
        omo[i] = mend_pix(g, x, y, W, H, op2(g, SpvOpISub, I, nkm[i], z1), ux, uy);
        omi[i] = mend_pix(g, x, y, W, H, op2(g, SpvOpIAdd, I, nkm[i], z1), ux, uy);
    }
    u32 vfar = cf(g, 1e30f);
    u32 best = z0, bestk = z0;
    for (int i = 0; i < NJ; i++)
        for (int j = 0; j < NJ; j++) {
            u32 t = op2(g, SpvOpISub, I, op2(g, SpvOpIAdd, I, kp[i], km[j]), z1);
            u32 valid = band(g, band(g, op2(g, SpvOpSGreaterThan, B, kp[i], z0), op2(g, SpvOpSGreaterThan, B, km[j], z0)),
                             op2(g, SpvOpSLessThanEqual, B, t, kmax));
            u32 better = op2(g, SpvOpLogicalOr, B, op2(g, SpvOpIEqual, B, best, z0), op2(g, SpvOpSLessThan, B, t, best));
            /* flood_line_agree */
            u32 gg = fmin_(g, mend_diff(g, op[i], opo[i]), mend_diff(g, om[j], omo[j]));
            u32 d = mend_diff(g, op[i], om[j]);
            u32 size = op2(g, SpvOpFAdd, F, mend_abs(g, op[i].t), mend_abs(g, om[j].t));
            u32 t1 = op1(g, SpvOpConvertSToF, F, op2(g, SpvOpIAdd, I, t, z1));
            u32 lim = op2(g, SpvOpFAdd, F, op2(g, SpvOpFMul, F, op2(g, SpvOpFMul, F, cf(g, 4.0f), t1), gg),
                          op2(g, SpvOpFMul, F, cf(g, 1e-4f), size));
            u32 agree = band(g, op2(g, SpvOpFOrdLessThan, B, gg, vfar), op2(g, SpvOpFOrdLessThanEqual, B, d, lim));
            u32 take = band(g, band(g, valid, better), agree);
            best = isel(g, take, t, best);
            bestk = isel(g, take, isel(g, op2(g, SpvOpSLessThanEqual, B, kp[i], km[j]), kp[i], nkm[j]), bestk);
        }
    /* between regions that disagree: the nearest clean region on each side (flood_line_clean) */
    u32 cp = z0, cm = z0;
    for (int i = NJ - 1; i >= 0; i--) {
        u32 cleanp = band(g, op2(g, SpvOpSGreaterThan, B, kp[i], z0),
                          band(g, op2(g, SpvOpFOrdLessThan, B, mend_diff(g, op[i], opo[i]), vfar),
                               op2(g, SpvOpFOrdLessThanEqual, B, op2(g, SpvOpFMul, F, cf(g, 8.0f), mend_diff(g, op[i], opo[i])),
                                   mend_diff(g, opi[i], op[i]))));
        cp = isel(g, cleanp, kp[i], cp);
        u32 cleanm = band(g, op2(g, SpvOpSGreaterThan, B, km[i], z0),
                          band(g, op2(g, SpvOpFOrdLessThan, B, mend_diff(g, om[i], omo[i]), vfar),
                               op2(g, SpvOpFOrdLessThanEqual, B, op2(g, SpvOpFMul, F, cf(g, 8.0f), mend_diff(g, om[i], omo[i])),
                                   mend_diff(g, omi[i], om[i]))));
        cm = isel(g, cleanm, km[i], cm);
    }
    u32 tf = op2(g, SpvOpISub, I, op2(g, SpvOpIAdd, I, cp, cm), z1);
    u32 fall = band(g, op2(g, SpvOpIEqual, B, best, z0),
                    band(g, band(g, op2(g, SpvOpSGreaterThan, B, cp, z0), op2(g, SpvOpSGreaterThan, B, cm, z0)),
                         op2(g, SpvOpSLessThanEqual, B, tf, kmax)));
    best = isel(g, fall, op2(g, SpvOpIAdd, I, tf, ci(g, FLOOD_MEND_KMAX + 1)), best);
    bestk = isel(g, fall, isel(g, op2(g, SpvOpSLessThanEqual, B, cp, cm), cp, op1(g, SpvOpSNegate, I, cm)), bestk);
    u32 sx_ = op2(g, SpvOpIAdd, I, x, op2(g, SpvOpIMul, I, bestk, ux)), sy_ = op2(g, SpvOpIAdd, I, y, op2(g, SpvOpIMul, I, bestk, uy));
    *src = op2(g, SpvOpIAdd, I, op2(g, SpvOpIMul, I, sy_, W), sx_);
    return best;
}

/* flood_mend_source (the pixel is converged): 0 or 1 + the pixel to continue from */
static u32 mend_source(Gen *g, u32 x, u32 y, u32 W, u32 H, u32 sx, u32 sy) {
    u32 I = g->tint, B = g->tbool, z0 = ci(g, 0), z1 = ci(g, 1);
    /* kmax = clamp(min(w, h) / 40, 8, FLOOD_MEND_KMAX), as flood_mend_limit */
    u32 kmax = ext3(g, I, GLSLSClamp, op2(g, SpvOpSDiv, I, ext2(g, I, GLSLSMin, W, H), ci(g, 40)), ci(g, 8), ci(g, FLOOD_MEND_KMAX));
    /* across the ray from the start pixel: vertically when |x - sx| >= |y - sy| */
    u32 vertical = op2(g, SpvOpSGreaterThanEqual, B, iabs_(g, op2(g, SpvOpISub, I, x, sx)), iabs_(g, op2(g, SpvOpISub, I, y, sy)));
    u32 src;
    u32 t = mend_axis(g, x, y, W, H, isel(g, vertical, z0, z1), isel(g, vertical, z1, z0), kmax, &src);
    u32 r = isel(g, op2(g, SpvOpSGreaterThan, B, t, z0), op2(g, SpvOpIAdd, I, src, z1), z0);
    return op1(g, SpvOpBitcast, g->tuint, r);
}

/* The flood-fill prologue (see flood.c, which this must match exactly): find this invocation's
 * pixel, skip it unless this pass computes it, pick the neighbour to continue from, set the flood_*
 * variables.  A decide pass (mending) only writes the pixel's `pad` and returns.
 * Returns the pixel's (x, y) as ints through *px, *py. */
static void flood_prologue(Gen *g, u32 pc_var, u32 gid_var, u32 *out_x, u32 *out_y, u32 *out_w, u32 *out_h) {
    u32 I = g->tint, B = g->tbool, F = g->tfloat, U = g->tuint;
    u32 z0 = ci(g, 0), z1 = ci(g, 1), z2 = ci(g, 2), z3 = ci(g, 3);
    u32 size = load(g, g->tvec2, g_access(g, ptr(g, SpvSCPushConstant, g->tvec2), pc_var, &z0, 1));
    u32 ivec2 = spv_type_vec(&g->s, I, 2);
    u32 start = load(g, ivec2, g_access(g, ptr(g, SpvSCPushConstant, ivec2), pc_var, &z1, 1));
    u32 ring = load(g, I, g_access(g, ptr(g, SpvSCPushConstant, I), pc_var, &z2, 1));
    u32 mode = load(g, I, g_access(g, ptr(g, SpvSCPushConstant, I), pc_var, &z3, 1));
    store(g, g->flood_pass_var, mode);
    u32 W = op1(g, SpvOpConvertFToS, I, extract(g, F, size, 0)), H = op1(g, SpvOpConvertFToS, I, extract(g, F, size, 1));
    u32 sx = extract(g, I, start, 0), sy = extract(g, I, start, 1);
    u32 uvec3 = spv_type_vec(&g->s, g->tuint, 3);
    u32 gid = op1(g, SpvOpBitcast, I, extract(g, g->tuint, load(g, uvec3, gid_var), 0));
    u32 is_cold = op2(g, SpvOpIEqual, B, mode, ci(g, FLOOD_PASS_COLD));
    u32 is_ring = op2(g, SpvOpIEqual, B, mode, ci(g, FLOOD_PASS_RING));
    u32 is_decide = op2(g, SpvOpIEqual, B, mode, ci(g, FLOOD_PASS_DECIDE));
    u32 is_mend = op2(g, SpvOpIEqual, B, mode, ci(g, FLOOD_PASS_MEND));
    /* ring pass: the gid-th pixel of the ring; other passes: pixel gid in row order */
    u32 two_d = imax_(g, op2(g, SpvOpIMul, I, ci(g, 2), ring), ci(g, 1));
    u32 side = op2(g, SpvOpSDiv, I, gid, two_d);
    u32 j = op2(g, SpvOpISub, I, gid, op2(g, SpvOpIMul, I, side, two_d));
    u32 xm = op2(g, SpvOpISub, I, sx, ring), xp = op2(g, SpvOpIAdd, I, sx, ring);
    u32 ym = op2(g, SpvOpISub, I, sy, ring), yp = op2(g, SpvOpIAdd, I, sy, ring);
    u32 rx0 = op2(g, SpvOpIAdd, I, xm, j), ry1 = op2(g, SpvOpIAdd, I, ym, j);
    u32 rx2 = op2(g, SpvOpISub, I, xp, j), ry3 = op2(g, SpvOpISub, I, yp, j);
    u32 s0 = op2(g, SpvOpIEqual, B, side, z0), s1 = op2(g, SpvOpIEqual, B, side, z1), s2 = op2(g, SpvOpIEqual, B, side, z2);
    u32 rx = isel(g, s0, rx0, isel(g, s1, xp, isel(g, s2, rx2, xm)));
    u32 ry = isel(g, s0, ym, isel(g, s1, ry1, isel(g, s2, yp, ry3)));
    u32 ring_n = isel(g, op2(g, SpvOpIEqual, B, ring, z0), z1, op2(g, SpvOpIMul, I, ci(g, 8), ring));
    u32 cx = op2(g, SpvOpSMod, I, gid, imax_(g, W, z1)), cy = op2(g, SpvOpSDiv, I, gid, imax_(g, W, z1));
    u32 x = isel(g, is_ring, rx, cx), y = isel(g, is_ring, ry, cy);
    u32 count = isel(g, is_ring, ring_n, op2(g, SpvOpIMul, I, W, H));
    u32 outside = op2(g, SpvOpLogicalOr, B, op2(g, SpvOpSGreaterThanEqual, B, gid, count),
                      op2(g, SpvOpLogicalOr, B, op2(g, SpvOpLogicalOr, B, op2(g, SpvOpSLessThan, B, x, z0), op2(g, SpvOpSLessThan, B, y, z0)),
                          op2(g, SpvOpLogicalOr, B, op2(g, SpvOpSGreaterThanEqual, B, x, W), op2(g, SpvOpSGreaterThanEqual, B, y, H))));
    return_if(g, outside);
    u32 idx = op2(g, SpvOpIAdd, I, op2(g, SpvOpIMul, I, y, W), x);
    store(g, g->pixel_var, idx);
    u32 sti[3] = {z0, idx, z1}, padi[3] = {z0, idx, z2}; /* members: t, status, pad */
    u32 status = load(g, U, g_access(g, ptr(g, SpvSCUniform, U), g->field_var, sti, 3));
    u32 is_done = op2(g, SpvOpIEqual, B, status, spv_const_u32(&g->s, U, 1 /* FLOOD_DONE */));
    /* decide pass: pad = the pixel to continue from (+ 1) if this one is in a band to mend */
    {
        u32 yes = new_label(g), go = new_label(g);
        spv_selection_merge(&g->s, go);
        spv_branch_cond(&g->s, is_decide, yes, go);
        label(g, yes);
        u32 pad = spv_op3(&g->s, SpvOpSelect, U, is_done, mend_source(g, x, y, W, H, sx, sy), spv_const_u32(&g->s, U, 0));
        store(g, g_access(g, ptr(g, SpvSCUniform, U), g->field_var, padi, 3), pad);
        spv_void(&g->s, SpvOpReturn, NULL, 0);
        label(g, go);
    }
    /* mend pass: a converged pixel with a pad, whose source is converged and not being mended itself */
    u32 pad = load(g, U, g_access(g, ptr(g, SpvSCUniform, U), g->field_var, padi, 3));
    u32 u0 = spv_const_u32(&g->s, U, 0), last = op2(g, SpvOpISub, I, op2(g, SpvOpIMul, I, W, H), z1);
    u32 src = ext3(g, I, GLSLSClamp, op2(g, SpvOpISub, I, op1(g, SpvOpBitcast, I, pad), z1), z0, last);
    u32 srcpi[3] = {z0, src, z2};
    u32 src_pad = load(g, U, g_access(g, ptr(g, SpvSCUniform, U), g->field_var, srcpi, 3));
    for (int hop = 0; hop < FLOOD_MEND_HOPS; hop++) { /* the source is mended too: its source (...) */
        u32 next = ext3(g, I, GLSLSClamp, op2(g, SpvOpISub, I, op1(g, SpvOpBitcast, I, src_pad), z1), z0, last);
        u32 more = op2(g, SpvOpINotEqual, B, src_pad, u0);
        src = isel(g, more, next, src);
        u32 npi[3] = {z0, src, z2};
        src_pad = load(g, U, g_access(g, ptr(g, SpvSCUniform, U), g->field_var, npi, 3));
    }
    u32 srci[3] = {z0, src, z1};
    u32 src_st = load(g, U, g_access(g, ptr(g, SpvSCUniform, U), g->field_var, srci, 3));
    u32 mend_ok = band(g, band(g, is_done, op2(g, SpvOpINotEqual, B, pad, u0)),
                       band(g, op2(g, SpvOpIEqual, B, src_pad, u0), op2(g, SpvOpIEqual, B, src_st, spv_const_u32(&g->s, U, 1))));
    u32 waiting = op2(g, SpvOpIEqual, B, status, u0 /* FLOOD_PENDING */);
    return_if(g, bnot(g, op2(g, SpvOpLogicalOr, B, band(g, is_mend, mend_ok), band(g, bnot(g, is_mend), waiting))));
    /* neighbour: rings d-1 and d+1 only, converged, d-1 first, then closest to the ray from the start */
    static const int dxs[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int dys[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    u32 d = imax_(g, iabs_(g, op2(g, SpvOpISub, I, x, sx)), iabs_(g, op2(g, SpvOpISub, I, y, sy)));
    u32 best = ci(g, -1), best_score = ci(g, 0x7fffffff);
    u32 ysy = op2(g, SpvOpISub, I, y, sy), xsx = op2(g, SpvOpISub, I, x, sx);
    for (int k = 0; k < 8; k++) {
        u32 nx = op2(g, SpvOpIAdd, I, x, ci(g, dxs[k])), ny = op2(g, SpvOpIAdd, I, y, ci(g, dys[k]));
        u32 inb = op2(g, SpvOpLogicalAnd, B,
                      op2(g, SpvOpLogicalAnd, B, op2(g, SpvOpSGreaterThanEqual, B, nx, z0), op2(g, SpvOpSGreaterThanEqual, B, ny, z0)),
                      op2(g, SpvOpLogicalAnd, B, op2(g, SpvOpSLessThan, B, nx, W), op2(g, SpvOpSLessThan, B, ny, H)));
        u32 nxs = op2(g, SpvOpISub, I, nx, sx), nys = op2(g, SpvOpISub, I, ny, sy);
        u32 dn = imax_(g, iabs_(g, nxs), iabs_(g, nys));
        u32 safe = op2(g, SpvOpIAdd, I, op2(g, SpvOpIMul, I, ext3(g, I, GLSLSClamp, ny, z0, op2(g, SpvOpISub, I, H, z1)), W),
                       ext3(g, I, GLSLSClamp, nx, z0, op2(g, SpvOpISub, I, W, z1)));
        u32 nsi[3] = {z0, safe, z1};
        u32 nst = load(g, g->tuint, g_access(g, ptr(g, SpvSCUniform, g->tuint), g->field_var, nsi, 3));
        u32 done = op2(g, SpvOpIEqual, B, nst, spv_const_u32(&g->s, g->tuint, 1 /* FLOOD_DONE */));
        u32 cross = iabs_(g, op2(g, SpvOpISub, I, op2(g, SpvOpIMul, I, nxs, ysy), op2(g, SpvOpIMul, I, nys, xsx)));
        u32 score = op2(g, SpvOpIAdd, I, isel(g, op2(g, SpvOpSLessThan, B, dn, d), z0, ci(g, 1 << 20)), cross);
        u32 ok = op2(g, SpvOpLogicalAnd, B, op2(g, SpvOpLogicalAnd, B, inb, op2(g, SpvOpINotEqual, B, dn, d)),
                     op2(g, SpvOpLogicalAnd, B, done, op2(g, SpvOpSLessThan, B, score, best_score)));
        best_score = isel(g, ok, score, best_score);
        best = isel(g, ok, safe, best);
    }
    u32 has_nb = op2(g, SpvOpSGreaterThanEqual, B, best, z0);
    u32 at_start = op2(g, SpvOpLogicalAnd, B, op2(g, SpvOpIEqual, B, x, sx), op2(g, SpvOpIEqual, B, y, sy));
    /* a ring pixel without a converged neighbour waits (unless it is the start) */
    return_if(g, op2(g, SpvOpLogicalAnd, B, is_ring,
                     op2(g, SpvOpLogicalAnd, B, op1(g, SpvOpLogicalNot, B, has_nb), op1(g, SpvOpLogicalNot, B, at_start))));
    u32 fmode = isel(g, is_mend, ci(g, FLOOD_CONTINUE),
                     isel(g, is_cold, ci(g, FLOOD_DIRECT), isel(g, has_nb, ci(g, FLOOD_CONTINUE), ci(g, FLOOD_START))));
    store(g, g->flood_mode_var, fmode);
    u32 nb = isel(g, is_mend, src, imax_(g, best, z0));
    u32 nti[3] = {z0, nb, z0};
    store(g, g->flood_seed_var, load(g, g->tvec2, g_access(g, ptr(g, SpvSCUniform, g->tvec2), g->field_var, nti, 3)));
    *out_x = x;
    *out_y = y;
    *out_w = extract(g, F, size, 0);
    *out_h = extract(g, F, size, 1);
    /* the neighbour's point is computed after the globals are loaded (flood_from is set by the caller) */
    g->flood_nb = nb;
    g->flood_W = W;
}

/* Generate the shader of viewer `v`: a fragment shader, or (compute) the flood-fill compute shader. */
static u32 *codegen_viewer_impl(Program *p, Section *v, bool compute, bool df, u32 *nwords, char *err, int errcap) {
    Gen G = {0};
    Gen *g = &G;
    g->prog = p;
    g->viewer = v;
    g->compute = compute;
    g->df = df && !v->mobius; /* (projective viewers stay in single precision) */
    reset_ids(p);
    spv_init(&g->s);
    Spv *s = &g->s;
    g->tvoid = spv_type_void(s);
    g->tbool = spv_type_bool(s);
    g->tint = spv_type_int(s, true);
    g->tuint = spv_type_int(s, false);
    g->tfloat = spv_type_float(s);
    g->tvec2 = spv_type_vec(s, g->tfloat, 2);
    g->tvec4 = spv_type_vec(s, g->tfloat, 4);
    g->treal = g->df ? g->tvec2 : g->tfloat;
    g->tcplx = g->df ? g->tvec4 : g->tvec2;

    /* which globals does the viewer use? */
    mark_block(g, &v->body);
    p->s_max_iter->used = p->s_epsilon->used = p->s_escape->used = true;
    v->center->used = v->radius->used = true;
    if (g->df && v->center_lo) v->center_lo->used = true;
    if (v->mobius) v->mobius->used = true;

    /* state buffer: struct of every global (layout from check.c) */
    int nmem = 0;
    u32 *members = (u32 *)xmalloc(sizeof(u32) * (size_t)(p->globals.len + 1));
    for (int i = 0; i < p->globals.len; i++) members[nmem++] = bty(g, p->globals.data[i]->type);
    u32 st = spv_type_struct(s, members, nmem);
    spv_decorate(s, st, SpvDecBufferBlock, NULL, 0);
    for (int i = 0; i < nmem; i++) {
        spv_member_decorate(s, st, (u32)i, SpvDecOffset, (u32)p->globals.data[i]->slot * 4, true);
        spv_member_decorate(s, st, (u32)i, SpvDecNonWritable, 0, false);
    }
    free(members);
    g->state_var = spv_global_var(s, ptr(g, SpvSCUniform, st), SpvSCUniform, 0);
    spv_decorate1(s, g->state_var, SpvDecDescriptorSet, 0);
    spv_decorate1(s, g->state_var, SpvDecBinding, 0);
    spv_name(s, g->state_var, "state");

    u32 gid_var = 0;
    if (!compute) {
        u32 pcm[2] = {g->tvec2, g->tvec2};
        u32 pct = spv_type_struct(s, pcm, 2);
        spv_decorate(s, pct, SpvDecBlock, NULL, 0);
        spv_member_decorate(s, pct, 0, SpvDecOffset, 0, true);
        spv_member_decorate(s, pct, 1, SpvDecOffset, 8, true);
        g->pc_var = spv_global_var(s, ptr(g, SpvSCPushConstant, pct), SpvSCPushConstant, 0);
        g->fragcoord = spv_global_var(s, ptr(g, SpvSCInput, g->tvec4), SpvSCInput, 0);
        spv_decorate1(s, g->fragcoord, SpvDecBuiltIn, SpvBuiltInFragCoord);
        g->out_color = spv_global_var(s, ptr(g, SpvSCOutput, g->tvec4), SpvSCOutput, 0);
        spv_decorate1(s, g->out_color, SpvDecLocation, 0);
    } else {
        /* push constants: vec2 size, ivec2 start, int ring, int mode */
        u32 ivec2 = spv_type_vec(s, g->tint, 2);
        u32 pcm[4] = {g->tvec2, ivec2, g->tint, g->tint};
        u32 pct = spv_type_struct(s, pcm, 4);
        spv_decorate(s, pct, SpvDecBlock, NULL, 0);
        static const u32 offs[4] = {0, 8, 16, 20};
        for (int i = 0; i < 4; i++) spv_member_decorate(s, pct, (u32)i, SpvDecOffset, offs[i], true);
        g->pc_var = spv_global_var(s, ptr(g, SpvSCPushConstant, pct), SpvSCPushConstant, 0);
        u32 uvec3 = spv_type_vec(s, g->tuint, 3);
        gid_var = spv_global_var(s, ptr(g, SpvSCInput, uvec3), SpvSCInput, 0);
        spv_decorate1(s, gid_var, SpvDecBuiltIn, SpvBuiltInGlobalInvocationId);
        /* binding 1: field entries {vec2 t; uint status; uint pad}; binding 2: packed RGBA8 colours */
        u32 em[3] = {g->tvec2, g->tuint, g->tuint};
        u32 entry = spv_type_struct(s, em, 3);
        spv_member_decorate(s, entry, 0, SpvDecOffset, 0, true);
        spv_member_decorate(s, entry, 1, SpvDecOffset, 8, true);
        spv_member_decorate(s, entry, 2, SpvDecOffset, 12, true);
        u32 earr = spv_type_runtime_array(s, entry);
        spv_decorate1(s, earr, SpvDecArrayStride, 16);
        u32 fst = spv_type_struct(s, &earr, 1);
        spv_decorate(s, fst, SpvDecBufferBlock, NULL, 0);
        spv_member_decorate(s, fst, 0, SpvDecOffset, 0, true);
        g->field_var = spv_global_var(s, ptr(g, SpvSCUniform, fst), SpvSCUniform, 0);
        spv_decorate1(s, g->field_var, SpvDecDescriptorSet, 0);
        spv_decorate1(s, g->field_var, SpvDecBinding, 1);
        spv_name(s, g->field_var, "field");
        u32 carr = spv_type_runtime_array(s, g->tuint);
        spv_decorate1(s, carr, SpvDecArrayStride, 4);
        u32 cst = spv_type_struct(s, &carr, 1);
        spv_decorate(s, cst, SpvDecBufferBlock, NULL, 0);
        spv_member_decorate(s, cst, 0, SpvDecOffset, 0, true);
        g->colors_var = spv_global_var(s, ptr(g, SpvSCUniform, cst), SpvSCUniform, 0);
        spv_decorate1(s, g->colors_var, SpvDecDescriptorSet, 0);
        spv_decorate1(s, g->colors_var, SpvDecBinding, 2);
        spv_name(s, g->colors_var, "colors");
        /* per-invocation flood state */
        g->flood_mode_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tint), SpvSCPrivate, ci(g, FLOOD_DIRECT));
        g->flood_seed_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tvec2), SpvSCPrivate, spv_const_null(s, g->tvec2));
        g->flood_from_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tcplx), SpvSCPrivate, spv_const_null(s, g->tcplx));
        g->flood_out_t_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tvec2), SpvSCPrivate, spv_const_null(s, g->tvec2));
        g->flood_out_ok_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tint), SpvSCPrivate, ci(g, -1));
        g->flood_pass_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tint), SpvSCPrivate, ci(g, 0));
        g->pixel_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tint), SpvSCPrivate, ci(g, 0));
    }

    /* Private copies of the used globals and the per-run internals */
    for (int i = 0; i < p->globals.len; i++) {
        Sym *gs = p->globals.data[i];
        if (!gs->used) continue;
        gs->spv_var = spv_global_var(s, ptr(g, SpvSCPrivate, ty(g, gs->type)), SpvSCPrivate, 0);
        spv_name_safe(s, gs->spv_var, "g_", gs->name);
    }
    g->stuck_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tbool), SpvSCPrivate, spv_const_bool(s, false));
    g->iter_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->tint), SpvSCPrivate, ci(g, 0));
    g->pxsize_var = spv_global_var(s, ptr(g, SpvSCPrivate, g->treal), SpvSCPrivate, spv_const_null(s, g->treal));
    p->s_stuck->spv_var = g->stuck_var;
    p->s_iterations->spv_var = g->iter_var;
    p->s_pixel->spv_var = g->pxsize_var;

    /* main */
    u32 mainfn = spv_new_id(s);
    u32 pids[1];
    spv_begin_function(s, mainfn, g->tvoid, spv_type_fn(s, g->tvoid, NULL, 0), NULL, pids, 0);
    spv_name(s, mainfn, "main");
    for (int i = 0; i < v->locals.len; i++) {
        Sym *ls = v->locals.data[i];
        ls->spv_var = local_var(g, ls->type);
        spv_name_safe(s, ls->spv_var, "v_", ls->name);
    }
    u32 fx = 0, fy = 0, fw_ = 0, fh_ = 0;
    if (compute) flood_prologue(g, g->pc_var, gid_var, &fx, &fy, &fw_, &fh_);
    for (int i = 0; i < p->globals.len; i++) {
        Sym *gs = p->globals.data[i];
        if (!gs->used) continue;
        u32 idx = ci(g, i);
        u32 bp = g_access(g, ptr(g, SpvSCUniform, bty(g, gs->type)), g->state_var, &idx, 1);
        store(g, gs->spv_var, from_buffer(g, gs->type, load(g, bty(g, gs->type), bp)));
    }
    if (compute) {
        /* this pixel's point and the neighbour's point */
        u32 f = g->tfloat, half = cf(g, 0.5f);
        u32 px = op2(g, SpvOpFAdd, f, op1(g, SpvOpConvertSToF, f, fx), half);
        u32 py = op2(g, SpvOpFAdd, f, op1(g, SpvOpConvertSToF, f, fy), half);
        u32 z = flat_plane_point(g, px, py, fw_, fh_);
        if (v->coord) store(g, v->coord->spv_var, z);
        store(g, g->pxsize_var, flat_pixel_size(g, fw_, fh_));
        u32 W = g->flood_W;
        u32 nx = op2(g, SpvOpSMod, g->tint, g->flood_nb, ext2(g, g->tint, GLSLSMax, W, ci(g, 1)));
        u32 ny = op2(g, SpvOpSDiv, g->tint, g->flood_nb, ext2(g, g->tint, GLSLSMax, W, ci(g, 1)));
        u32 npx = op2(g, SpvOpFAdd, f, op1(g, SpvOpConvertSToF, f, nx), half);
        u32 npy = op2(g, SpvOpFAdd, f, op1(g, SpvOpConvertSToF, f, ny), half);
        store(g, g->flood_from_var, flat_plane_point(g, npx, npy, fw_, fh_));
    } else {
        /* pixel -> plane */
        u32 f = g->tfloat;
        u32 fc = load(g, g->tvec4, g->fragcoord);
        u32 z0 = ci(g, 0), z1 = ci(g, 1);
        u32 size = load(g, g->tvec2, g_access(g, ptr(g, SpvSCPushConstant, g->tvec2), g->pc_var, &z0, 1));
        u32 off = load(g, g->tvec2, g_access(g, ptr(g, SpvSCPushConstant, g->tvec2), g->pc_var, &z1, 1));
        u32 px = op2(g, SpvOpFAdd, f, extract(g, f, fc, 0), extract(g, f, off, 0));
        u32 py = op2(g, SpvOpFAdd, f, extract(g, f, fc, 1), extract(g, f, off, 1));
        u32 w = extract(g, f, size, 0), h = extract(g, f, size, 1);
        u32 z, lift = 0;
        if (v->mobius) {
            lift = proj_plane_lift(g, px, py, w, h);
            z = g_cdiv(g, vec2(g, extract(g, f, lift, 0), extract(g, f, lift, 1)),
                       vec2(g, extract(g, f, lift, 2), extract(g, f, lift, 3)));
            /* .pixel: distance to the point of the next pixel */
            u32 z1 = proj_plane_point(g, op2(g, SpvOpFAdd, f, px, cf(g, 1)), py, w, h);
            store(g, g->pxsize_var, ext1(g, f, GLSLLength, op2(g, SpvOpFSub, g->tvec2, z1, z)));
        } else {
            z = flat_plane_point(g, px, py, w, h);
            store(g, g->pxsize_var, flat_pixel_size(g, w, h));
            if (v->var_proj) {
                u32 p[4] = {extract(g, f, z, 0), extract(g, f, z, 1), cf(g, 1), cf(g, 0)};
                lift = construct(g, g->tvec4, p, 4);
            }
        }
        if (v->coord) store(g, v->coord->spv_var, v->var_proj ? proj_scale(g, lift) : z); /* (z : P: exact at infinity) */
    }
    g->fn = NULL;
    g->dead = false;
    gen_block(g, &v->body);
    if (!g->dead) gen_return(g);
    spv_end_function(s);

    for (int qi = 0; qi < g->queue.len; qi++) emit_func(g, g->queue.data[qi]);

    if (compute) {
        spv_entry_point_compute(s, mainfn, &gid_var, 1, FLOOD_GROUP_SIZE);
    } else {
        u32 iface[2] = {g->fragcoord, g->out_color};
        spv_entry_point(s, mainfn, iface, 2);
    }
    u32 *words = spv_finish(s, nwords);
    spv_free(s);
    vec_free(g->queue);
    vec_free(g->visited);
    if (err && errcap > 0) err[0] = 0;
    return words;
}

/* Generate the fragment shader of viewer `v`.  Returns malloc'd words or NULL.  With `df`, reals and
 * complex numbers are computed in double-single precision (deep zooms; flat viewers only). */
u32 *codegen_viewer_ex(Program *p, Section *v, bool df, u32 *nwords, char *err, int errcap) {
    return codegen_viewer_impl(p, v, false, df, nwords, err, errcap);
}
u32 *codegen_viewer(Program *p, Section *v, u32 *nwords, char *err, int errcap) {
    return codegen_viewer_impl(p, v, false, false, nwords, err, errcap);
}

/* Generate the flood-fill compute shader of viewer `v` (it must have a `continuing` solve): one
 * invocation per pixel of a ring (or of the image for the other passes), see flood.c.
 * Bindings: 0 state, 1 field (FloodEntry[]), 2 colours (packed RGBA8 per pixel);
 * push constants: vec2 size, ivec2 start, int ring, int mode (FLOOD_PASS_*). */
u32 *codegen_viewer_flood_ex(Program *p, Section *v, bool df, u32 *nwords, char *err, int errcap) {
    if (!v->flood) {
        if (err) snprintf(err, (size_t)errcap, "this viewer has no 'continuing' solve");
        return NULL;
    }
    return codegen_viewer_impl(p, v, true, df, nwords, err, errcap);
}
u32 *codegen_viewer_flood(Program *p, Section *v, u32 *nwords, char *err, int errcap) {
    return codegen_viewer_flood_ex(p, v, false, nwords, err, errcap);
}
