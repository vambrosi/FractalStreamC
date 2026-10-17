/* interp.c - tree-walking interpreter over the checked AST.
 *
 * Values are arrays of 32-bit words in the std430 layout described in
 * ast.h, so the global state buffer can be uploaded to the GPU as is.
 * Arithmetic is done in float/int32 exactly like the shaders do, so the
 * CPU and GPU agree up to rounding in transcendental functions.
 *
 * Loops stop after .maxIterations iterations (setting .stuck); in
 * addition a run aborts after `step_limit` loop iterations in total so a
 * runaway tool cannot freeze the GUI. */
#include "ast.h"

typedef struct DrawItem {
    DrawShape shape;
    float p0[2], p1[2];
    float radius;
    bool radius_px;
    float color[3];
    float width;      /* pixels */
    int viewer;       /* -1: the viewer the tool ran in */
} DrawItem;
typedef VEC(DrawItem) DrawItemVec;

enum { SIG_NONE, SIG_BREAK, SIG_CONTINUE, SIG_RETURN, SIG_ABORT };

typedef struct Interp {
    Program *prog;
    u32 *state;
    u32 *stack;
    int sp, cap;
    u32 *frame;
    bool stuck;
    i32 iterations;
    int signal;
    DrawItemVec *draws;
    double steps, step_limit;
    int quiet; /* > 0 inside prelude functions: their loops don't touch .stuck/.iterations */
    char error[256];
} Interp;

static inline float wf(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static inline u32 fw(float f) { u32 w; memcpy(&w, &f, 4); return w; }
static inline i32 wi(u32 w) { return (i32)w; }

static void interp_fail(Interp *I, const char *msg) {
    if (I->signal != SIG_ABORT) snprintf(I->error, sizeof I->error, "%s", msg);
    I->signal = SIG_ABORT;
}

static u32 *push_words(Interp *I, int n) {
    if (n < 1) n = 1;
    if (I->sp + n > I->cap) {
        interp_fail(I, "the computation needs too much memory (interpreter stack overflow)");
        static u32 dummy[4096];
        memset(dummy, 0, sizeof dummy);
        return dummy; /* callers stop at the abort signal */
    }
    u32 *p = I->stack + I->sp;
    memset(p, 0, sizeof(u32) * (size_t)n);
    I->sp += n;
    return p;
}

static void ev(Interp *I, Expr *e, u32 *out);
static void exec_block(Interp *I, Block *b);

/* ---- numeric helpers ---------------------------------------------------- */
static void cmul(const float *a, const float *b, float *r) {
    float x = a[0] * b[0] - a[1] * b[1], y = a[0] * b[1] + a[1] * b[0];
    r[0] = x;
    r[1] = y;
}
static void cdiv(const float *a, const float *b, float *r) {
    float d = b[0] * b[0] + b[1] * b[1];
    float x = (a[0] * b[0] + a[1] * b[1]) / d, y = (a[1] * b[0] - a[0] * b[1]) / d;
    r[0] = x;
    r[1] = y;
}
static void qmul(const float *a, const float *b, float *r) {
    float w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    float x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    float y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    float z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    r[0] = w; r[1] = x; r[2] = y; r[3] = z;
}
static void qinv(const float *a, float *r) {
    float d = a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3];
    r[0] = a[0] / d; r[1] = -a[1] / d; r[2] = -a[2] / d; r[3] = -a[3] / d;
}
static i32 imod_floor(i32 a, i32 b) {
    if (b == 0) return 0;
    if (b == -1) return 0;
    i32 m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;
    return m;
}
static i32 idiv_floor(i32 a, i32 b) {
    if (b == 0) return 0;
    if (b == -1) return (i32)(0u - (u32)a);
    i32 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
static float fmod_floor(float x, float y) { return x - y * floorf(x / y); }

/* scalar op on one number of type t (Z, R, C, H): r = a op b */
static void num_op(Type *t, int op, const u32 *a, const u32 *b, u32 *r) {
    if (t->kind == T_INT) {
        u32 x = a[0], y = b[0];
        switch (op) {
        case TK_PLUS: r[0] = x + y; break;
        case TK_MINUS: r[0] = x - y; break;
        case TK_STAR: r[0] = (u32)((u64)x * (u64)y); break;
        case TK_MOD: r[0] = (u32)imod_floor(wi(x), wi(y)); break;
        default: r[0] = 0; break;
        }
        return;
    }
    if (t->kind == T_REAL) {
        float x = wf(a[0]), y = wf(b[0]), z = 0;
        switch (op) {
        case TK_PLUS: z = x + y; break;
        case TK_MINUS: z = x - y; break;
        case TK_STAR: z = x * y; break;
        case TK_SLASH: z = x / y; break;
        case TK_MOD: z = fmod_floor(x, y); break;
        }
        r[0] = fw(z);
        return;
    }
    int n = t->kind == T_COMPLEX ? 2 : 4;
    float A[4], B[4], R[4] = {0};
    for (int i = 0; i < n; i++) { A[i] = wf(a[i]); B[i] = wf(b[i]); }
    if (op == TK_PLUS) for (int i = 0; i < n; i++) R[i] = A[i] + B[i];
    else if (op == TK_MINUS) for (int i = 0; i < n; i++) R[i] = A[i] - B[i];
    else if (op == TK_STAR) { if (n == 2) cmul(A, B, R); else qmul(A, B, R); }
    else if (op == TK_SLASH) {
        if (n == 2) cdiv(A, B, R);
        else { float inv[4]; qinv(B, inv); qmul(A, inv, R); }
    }
    for (int i = 0; i < n; i++) r[i] = fw(R[i]);
}

/* squared norm of a value of type t (numbers and vectors) */
static float norm2(Type *t, const u32 *v) {
    switch (t->kind) {
    case T_INT: { float x = (float)wi(v[0]); return x * x; }
    case T_REAL: return wf(v[0]) * wf(v[0]);
    case T_COMPLEX: return wf(v[0]) * wf(v[0]) + wf(v[1]) * wf(v[1]);
    case T_QUAT: return wf(v[0]) * wf(v[0]) + wf(v[1]) * wf(v[1]) + wf(v[2]) * wf(v[2]) + wf(v[3]) * wf(v[3]);
    case T_VEC: {
        float s = 0;
        for (int i = 0; i < t->n; i++) s += norm2(t->elem, v + i * t->stride);
        return s;
    }
    default: return 0;
    }
}

static void convert_value(Type *from, Type *to, const u32 *a, u32 *r) {
    if (from == to) {
        memcpy(r, a, sizeof(u32) * (size_t)to->size);
        return;
    }
    if (type_is_number(from) && type_is_number(to)) {
        float v0 = from->kind == T_INT ? (float)wi(a[0]) : wf(a[0]);
        int nf = from->kind == T_COMPLEX ? 2 : from->kind == T_QUAT ? 4 : 1;
        int nt = to->kind == T_COMPLEX ? 2 : to->kind == T_QUAT ? 4 : 1;
        if (to->kind == T_INT) { r[0] = a[0]; return; }
        r[0] = fw(v0);
        for (int i = 1; i < nt; i++) r[i] = i < nf ? a[i] : 0;
        return;
    }
    if (from->kind == T_VEC && to->kind == T_VEC) {
        for (int i = 0; i < to->n; i++) convert_value(from->elem, to->elem, a + i * from->stride, r + i * to->stride);
        return;
    }
    if (from->kind == T_VEC && to->kind == T_LIST) {
        r[0] = (u32)from->n;
        for (int i = 0; i < from->n; i++) convert_value(from->elem, to->elem, a + i * from->stride, r + to->data_off + i * to->stride);
        return;
    }
    memcpy(r, a, sizeof(u32) * (size_t)FS_MIN(from->size, to->size));
}

static int tuple_offset(Type *t, int item) {
    int off = 0;
    for (int i = 0; i <= item; i++) {
        int a = t->items[i]->align;
        off = (off + a - 1) / a * a;
        if (i == item) return off;
        off += t->items[i]->size;
    }
    return off;
}

/* ---- variables ---------------------------------------------------------- */
static u32 exec_words[2];

static u32 *sym_addr(Interp *I, Sym *s) {
    switch (s->kind) {
    case SYM_GLOBAL: return I->state + s->slot;
    case SYM_LOCAL: return I->frame + s->slot;
    case SYM_EXEC:
        exec_words[0] = s == I->prog->s_stuck ? (u32)I->stuck : (u32)I->iterations;
        return exec_words;
    case SYM_CONST: return NULL;
    }
    return NULL;
}

static void load_sym(Interp *I, Sym *s, u32 *out) {
    if (s->kind == SYM_CONST) {
        Type *t = s->type;
        if (t->kind == T_INT) out[0] = (u32)(i32)s->cval[0];
        else if (t->kind == T_VEC) for (int i = 0; i < t->n; i++) out[i * t->stride] = fw(s->cval[i]);
        else for (int i = 0; i < t->size; i++) out[i] = fw(s->cval[i]);
        return;
    }
    if (s->kind == SYM_EXEC) {
        out[0] = s == I->prog->s_stuck ? (u32)I->stuck : (u32)I->iterations;
        return;
    }
    memcpy(out, sym_addr(I, s), sizeof(u32) * (size_t)s->type->size);
}

static i32 clamp_index(i32 k, i32 n) { return k < 1 ? 1 : k > n ? n : k; }

/* address of an lvalue (NULL if it has none, e.g. an empty list) */
static u32 *lv_addr(Interp *I, Expr *e) {
    if (e->kind == E_NAME || e->kind == E_INTERNAL || e->kind == E_MEMBER) return sym_addr(I, e->sym);
    if (e->kind == E_INDEX) {
        u32 *base = lv_addr(I, e->a);
        if (!base) return NULL;
        u32 k;
        ev(I, e->b, &k);
        Type *bt = e->a->type;
        i32 idx = wi(k);
        switch (bt->kind) {
        case T_VEC: return base + (clamp_index(idx, bt->n) - 1) * bt->stride;
        case T_COMPLEX: return base + clamp_index(idx, 2) - 1;
        case T_QUAT: return base + clamp_index(idx, 4) - 1;
        case T_LIST: {
            i32 len = wi(base[0]);
            if (len <= 0) return NULL;
            return base + bt->data_off + (clamp_index(idx, len) - 1) * bt->stride;
        }
        default: return NULL;
        }
    }
    return NULL;
}

/* ---- expressions --------------------------------------------------------- */
static float fcall1(int id, float x) {
    switch (id) {
    case I_SIN: return sinf(x);
    case I_COS: return cosf(x);
    case I_TAN: return tanf(x);
    case I_ASIN: return asinf(x);
    case I_ACOS: return acosf(x);
    case I_ATAN: return atanf(x);
    case I_SINH: return sinhf(x);
    case I_COSH: return coshf(x);
    case I_TANH: return tanhf(x);
    case I_ASINH: return asinhf(x);
    case I_ACOSH: return acoshf(x);
    case I_ATANH: return atanhf(x);
    case I_EXP: return expf(x);
    case I_LOG: return logf(x);
    case I_SQRT: return sqrtf(x);
    case I_FLOOR: return floorf(x);
    case I_CEIL: return ceilf(x);
    case I_FRACT: return x - floorf(x);
    case I_FABS: return fabsf(x);
    case I_FSIGN: return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f;
    case I_EXP2: return exp2f(x);
    case I_LOG2: return log2f(x);
    case I_TRUNC: return truncf(x);
    }
    return 0;
}

static i32 f2i(float x) {
    if (!(x == x)) return 0;
    if (x >= 2147483647.0f) return 2147483647;
    if (x <= -2147483648.0f) return (i32)0x80000000u;
    return (i32)x;
}

static void ev_intrinsic(Interp *I, Expr *e, u32 *out) {
    u32 a[4] = {0}, b[4] = {0}, c[4] = {0};
    int id = e->intrinsic;
    if (id == I_APPEND || id == I_CLEAR) {
        u32 *L = lv_addr(I, e->args[0]);
        Type *lt = e->args[0]->type;
        if (!L) return;
        if (id == I_CLEAR) { L[0] = 0; return; }
        u32 *v = push_words(I, lt->elem->size);
        ev(I, e->args[1], v);
        i32 len = wi(L[0]);
        if (len < lt->n) {
            memcpy(L + lt->data_off + len * lt->stride, v, sizeof(u32) * (size_t)lt->elem->size);
            L[0] = (u32)(len + 1);
        }
        I->sp -= lt->elem->size > 0 ? lt->elem->size : 1;
        return;
    }
    if (id == I_LENGTH) {
        Type *t = e->args[0]->type;
        if (t->kind == T_VEC) { out[0] = (u32)t->n; return; }
        u32 *tmp = push_words(I, t->size);
        ev(I, e->args[0], tmp);
        out[0] = tmp[0];
        I->sp -= t->size;
        return;
    }
    if (id == I_DOT) {
        Type *t = e->args[0]->type;
        u32 *x = push_words(I, t->size), *y = push_words(I, t->size);
        ev(I, e->args[0], x);
        ev(I, e->args[1], y);
        Type *el = t->elem;
        u32 acc[4] = {0}, prod[4];
        if (el->kind != T_INT) acc[0] = fw(0);
        for (int i = 0; i < t->n; i++) {
            num_op(el, TK_STAR, x + i * t->stride, y + i * t->stride, prod);
            num_op(el, TK_PLUS, acc, prod, acc);
        }
        memcpy(out, acc, sizeof(u32) * (size_t)el->size);
        I->sp -= 2 * t->size;
        return;
    }
    if (e->nargs > 0) ev(I, e->args[0], a);
    if (e->nargs > 1) ev(I, e->args[1], b);
    if (e->nargs > 2) ev(I, e->args[2], c);
    float x = wf(a[0]), y = wf(b[0]), z = wf(c[0]);
    switch (id) {
    case I_ATAN2: out[0] = fw(atan2f(x, y)); return;
    case I_POW: out[0] = fw(powf(x, y)); return;
    case I_FMIN: out[0] = fw(fminf(x, y)); return;
    case I_FMAX: out[0] = fw(fmaxf(x, y)); return;
    case I_FMOD: out[0] = fw(fmod_floor(x, y)); return;
    case I_STEP: out[0] = fw(y < x ? 0.0f : 1.0f); return;
    case I_FCLAMP: out[0] = fw(fminf(fmaxf(x, y), z)); return;
    case I_FMIX: out[0] = fw(x * (1 - z) + y * z); return;
    case I_SMOOTHSTEP: {
        float t = fminf(fmaxf((z - x) / (y - x), 0.0f), 1.0f);
        out[0] = fw(t * t * (3 - 2 * t));
        return;
    }
    case I_IABS: out[0] = wi(a[0]) < 0 ? 0u - a[0] : a[0]; return;
    case I_ISIGN: out[0] = (u32)(wi(a[0]) > 0 ? 1 : wi(a[0]) < 0 ? -1 : 0); return;
    case I_IMIN: out[0] = (u32)FS_MIN(wi(a[0]), wi(b[0])); return;
    case I_IMAX: out[0] = (u32)FS_MAX(wi(a[0]), wi(b[0])); return;
    case I_IMOD: out[0] = (u32)imod_floor(wi(a[0]), wi(b[0])); return;
    case I_IDIV: out[0] = (u32)idiv_floor(wi(a[0]), wi(b[0])); return;
    case I_FLOORI: out[0] = (u32)f2i(floorf(x)); return;
    case I_CEILI: out[0] = (u32)f2i(ceilf(x)); return;
    case I_ROUNDI: out[0] = (u32)f2i(floorf(x + 0.5f)); return;
    case I_TRUNCI: out[0] = (u32)f2i(x); return;
    case I_COMPLEX: out[0] = a[0]; out[1] = b[0]; return;
    case I_RE: out[0] = a[0]; return;
    case I_IM: out[0] = a[1]; return;
    case I_QUAT: {
        u32 d[1];
        ev(I, e->args[3], d);
        out[0] = a[0]; out[1] = b[0]; out[2] = c[0]; out[3] = d[0];
        return;
    }
    case I_ISNAN: out[0] = x != x; return;
    case I_ISINF: out[0] = isinf(x) ? 1u : 0u; return;
    default: out[0] = fw(fcall1(id, x)); return;
    }
}

static void call_func(Interp *I, Func *f, Expr **args, u32 *out) {
    int mark = I->sp;
    u32 *nf = push_words(I, f->frame_words);
    for (int i = 0; i < f->nparams; i++) ev(I, args[i], nf + f->params[i]->slot);
    if (I->signal == SIG_ABORT) { I->sp = mark; return; }
    u32 *saved = I->frame;
    I->frame = nf;
    if (f->prelude) I->quiet++;
    if (f->body_expr) {
        ev(I, f->body_expr, out);
    } else {
        exec_block(I, &f->body);
        if (I->signal == SIG_RETURN) I->signal = SIG_NONE;
        if (f->noutputs == 1) {
            memcpy(out, nf + f->outputs[0]->slot, sizeof(u32) * (size_t)f->outputs[0]->type->size);
        } else {
            for (int i = 0; i < f->noutputs; i++)
                memcpy(out + tuple_offset(f->ret, i), nf + f->outputs[i]->slot, sizeof(u32) * (size_t)f->outputs[i]->type->size);
        }
    }
    if (f->prelude) I->quiet--;
    I->frame = saved;
    I->sp = mark;
}

static void powi(Type *t, const u32 *base, i32 n, u32 *out) {
    u32 one[4] = {0};
    if (t->kind == T_INT) one[0] = 1;
    else one[0] = fw(1.0f);
    u32 acc[4], b[4];
    memcpy(acc, one, sizeof acc);
    memcpy(b, base, sizeof(u32) * (size_t)t->size);
    u32 m = n < 0 ? 0u - (u32)n : (u32)n;
    while (m) {
        if (m & 1) num_op(t, TK_STAR, acc, b, acc);
        m >>= 1;
        if (m) num_op(t, TK_STAR, b, b, b);
    }
    if (n < 0 && t->kind != T_INT) num_op(t, TK_SLASH, one, acc, acc);
    memcpy(out, acc, sizeof(u32) * (size_t)t->size);
}

static void ev_binary(Interp *I, Expr *e, u32 *out) {
    int op = e->op;
    if (op == TK_AND || op == TK_OR) {
        u32 a;
        ev(I, e->a, &a);
        if (op == TK_AND && !a) { out[0] = 0; return; }
        if (op == TK_OR && a) { out[0] = 1; return; }
        ev(I, e->b, out);
        return;
    }
    Type *ta = e->a->type, *tb = e->b->type;
    int mark = I->sp;
    u32 *a = push_words(I, ta->size), *b = push_words(I, tb->size);
    ev(I, e->a, a);
    ev(I, e->b, b);
    if (op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE) {
        bool r;
        if (ta->kind == T_INT) {
            i32 x = wi(a[0]), y = wi(b[0]);
            r = op == TK_LT ? x < y : op == TK_LE ? x <= y : op == TK_GT ? x > y : x >= y;
        } else {
            float x = wf(a[0]), y = wf(b[0]);
            r = op == TK_LT ? x < y : op == TK_LE ? x <= y : op == TK_GT ? x > y : x >= y;
        }
        out[0] = r;
    } else if (op == TK_EQ || op == TK_NE) {
        bool eq;
        if (ta->kind == T_BOOL || ta->kind == T_INT) {
            eq = a[0] == b[0];
        } else {
            u32 *d = push_words(I, ta->size);
            if (ta->kind == T_VEC)
                for (int i = 0; i < ta->n; i++) num_op(ta->elem, TK_MINUS, a + i * ta->stride, b + i * ta->stride, d + i * ta->stride);
            else
                num_op(ta, TK_MINUS, a, b, d);
            float eps = wf(I->state[I->prog->s_epsilon->slot]);
            float n2 = norm2(ta, d);
            eq = n2 < eps * eps || n2 == 0;
        }
        out[0] = op == TK_EQ ? eq : !eq;
    } else if (ta->kind == T_VEC || tb->kind == T_VEC) {
        Type *rt = e->type, *el = rt->elem;
        for (int i = 0; i < rt->n; i++) {
            const u32 *x = ta->kind == T_VEC ? a + i * ta->stride : a;
            const u32 *y = tb->kind == T_VEC ? b + i * tb->stride : b;
            num_op(el, op, x, y, out + i * rt->stride);
        }
    } else {
        num_op(ta, op, a, b, out);
    }
    I->sp = mark;
}

static void ev(Interp *I, Expr *e, u32 *out) {
    if (I->signal == SIG_ABORT) return;
    switch (e->kind) {
    case E_INT: out[0] = (u32)e->ival; return;
    case E_REAL: out[0] = fw((float)e->rval); return;
    case E_IMAG: out[0] = fw(0); out[1] = fw((float)e->rval); return;
    case E_BOOL: out[0] = (u32)e->ival; return;
    case E_STRING: return;
    case E_NAME: case E_INTERNAL: case E_MEMBER: load_sym(I, e->sym, out); return;
    case E_CONVERT: {
        int mark = I->sp;
        u32 *a = push_words(I, e->a->type->size);
        ev(I, e->a, a);
        convert_value(e->a->type, e->type, a, out);
        I->sp = mark;
        return;
    }
    case E_UNARY: {
        ev(I, e->a, out);
        Type *t = e->type;
        if (e->op == TK_NOT) { out[0] = !out[0]; return; }
        Type *el = t->kind == T_VEC ? t->elem : t;
        int count = t->kind == T_VEC ? t->n : 1;
        for (int i = 0; i < count; i++) {
            u32 *v = out + i * (t->kind == T_VEC ? t->stride : 0);
            if (el->kind == T_INT) v[0] = 0u - v[0];
            else for (int k = 0; k < el->size; k++) v[k] = fw(-wf(v[k]));
        }
        return;
    }
    case E_BINARY: ev_binary(I, e, out); return;
    case E_POWI: {
        int mark = I->sp;
        u32 *a = push_words(I, e->a->type->size);
        ev(I, e->a, a);
        powi(e->type, a, e->ival, out);
        I->sp = mark;
        return;
    }
    case E_ABS: {
        Type *t = e->a->type;
        int mark = I->sp;
        u32 *a = push_words(I, t->size);
        ev(I, e->a, a);
        if (t->kind == T_INT) out[0] = wi(a[0]) < 0 ? 0u - a[0] : a[0];
        else if (t->kind == T_REAL) out[0] = fw(fabsf(wf(a[0])));
        else if (t->kind == T_LIST) out[0] = a[0];
        else out[0] = fw(sqrtf(norm2(t, a)));
        I->sp = mark;
        return;
    }
    case E_PRED: {
        Type *t = e->a->type;
        int mark = I->sp;
        u32 *a = push_words(I, t->size);
        ev(I, e->a, a);
        float n2 = norm2(t, a);
        if (e->op == TK_ESCAPES) {
            float r = wf(I->state[I->prog->s_escape->slot]);
            out[0] = n2 > r * r;
        } else {
            float eps = wf(I->state[I->prog->s_epsilon->slot]);
            out[0] = n2 < eps * eps || n2 == 0;
        }
        I->sp = mark;
        return;
    }
    case E_CALL:
        if (e->intrinsic) ev_intrinsic(I, e, out);
        else call_func(I, e->fn, e->args, out);
        return;
    case E_INDEX: {
        Type *bt = e->a->type;
        int mark = I->sp;
        u32 *base = push_words(I, bt->size);
        ev(I, e->a, base);
        u32 k;
        ev(I, e->b, &k);
        i32 idx = wi(k);
        switch (bt->kind) {
        case T_VEC: memcpy(out, base + (clamp_index(idx, bt->n) - 1) * bt->stride, sizeof(u32) * (size_t)bt->elem->size); break;
        case T_COMPLEX: out[0] = base[clamp_index(idx, 2) - 1]; break;
        case T_QUAT: out[0] = base[clamp_index(idx, 4) - 1]; break;
        case T_LIST: {
            i32 len = wi(base[0]);
            if (len > 0) memcpy(out, base + bt->data_off + (clamp_index(idx, len) - 1) * bt->stride, sizeof(u32) * (size_t)bt->elem->size);
            else memset(out, 0, sizeof(u32) * (size_t)bt->elem->size);
            break;
        }
        default: break;
        }
        I->sp = mark;
        return;
    }
    case E_VECTOR: {
        Type *t = e->type;
        if (t->kind == T_LIST) {
            memset(out, 0, sizeof(u32) * (size_t)t->size);
            out[0] = (u32)e->nargs;
            for (int i = 0; i < e->nargs; i++) ev(I, e->args[i], out + t->data_off + i * t->stride);
        } else {
            for (int i = 0; i < e->nargs; i++) ev(I, e->args[i], out + i * t->stride);
        }
        return;
    }
    case E_TUPLE:
        for (int i = 0; i < e->nargs; i++) ev(I, e->args[i], out + tuple_offset(e->type, i));
        return;
    case E_COLOR_CONST:
        for (int i = 0; i < e->type->size; i++) out[i] = fw(e->cval[i]);
        return;
    }
}

/* ---- statements ---------------------------------------------------------- */
static void store_value(Interp *I, Expr *target, const u32 *v) {
    u32 *dst = lv_addr(I, target);
    if (!dst) return;
    if (target->sym && target->sym->kind == SYM_EXEC) return;
    memcpy(dst, v, sizeof(u32) * (size_t)target->type->size);
}

static bool loop_tick(Interp *I) {
    I->steps += 1;
    if (I->steps > I->step_limit) {
        interp_fail(I, "the computation took too long and was stopped (too many loop iterations in total)");
        return false;
    }
    return true;
}

/* after a loop body: returns true if the loop must exit */
static bool after_body(Interp *I) {
    if (I->signal == SIG_BREAK) { I->signal = SIG_NONE; return true; }
    if (I->signal == SIG_CONTINUE) I->signal = SIG_NONE;
    return I->signal != SIG_NONE; /* return / abort */
}

static void exec_stmt(Interp *I, Stmt *s) {
    int mark = I->sp;
    i32 maxit = wi(I->state[I->prog->s_max_iter->slot]);
    switch (s->kind) {
    case S_DEFINE: case S_APPEND: break;
    case S_EXPR:
        if (s->value) {
            u32 *tmp = push_words(I, s->value->type->size);
            ev(I, s->value, tmp);
        }
        break;
    case S_DECL: case S_ASSIGN: {
        if (!s->target->sym && s->target->kind != E_INDEX) break;
        u32 *tmp = push_words(I, s->target->type->size);
        if (s->value) ev(I, s->value, tmp);
        if (I->signal != SIG_ABORT) store_value(I, s->target, tmp);
        break;
    }
    case S_DESTRUCTURE: {
        Type *tt = s->value->type;
        u32 *tmp = push_words(I, tt->size);
        ev(I, s->value, tmp);
        if (I->signal == SIG_ABORT) break;
        for (int i = 0; i < s->ntargets; i++) {
            Expr *tg = s->targets[i];
            u32 *cv = push_words(I, tg->type->size);
            convert_value(tt->items[i], tg->type, tmp + tuple_offset(tt, i), cv);
            store_value(I, tg, cv);
        }
        break;
    }
    case S_IF: {
        u32 c;
        ev(I, s->value, &c);
        if (I->signal == SIG_ABORT) break;
        exec_block(I, c ? &s->body : &s->else_body);
        break;
    }
    case S_WHILE: {
        i32 n = 0;
        bool stuck = false;
        for (;;) {
            u32 c;
            bool ok = n < maxit;
            ev(I, s->value, &c);
            if (I->signal == SIG_ABORT) break;
            stuck = !ok && c;
            if (!(ok && c)) break;
            n++;
            if (!loop_tick(I)) break;
            exec_block(I, &s->body);
            if (after_body(I)) break;
        }
        if (I->signal == SIG_NONE && !I->quiet) { I->iterations = n; I->stuck = stuck; }
        break;
    }
    case S_DOWHILE: {
        i32 n = 0;
        bool stuck = false;
        for (;;) {
            n++;
            if (!loop_tick(I)) break;
            exec_block(I, &s->body);
            if (after_body(I)) break;
            u32 c;
            bool ok = n < maxit;
            ev(I, s->value, &c);
            if (I->signal == SIG_ABORT) break;
            stuck = !ok && c;
            if (!(ok && c)) break;
        }
        if (I->signal == SIG_NONE && !I->quiet) { I->iterations = n; I->stuck = stuck; }
        break;
    }
    case S_FOR: {
        u32 lo, hi, st = 1;
        ev(I, s->from, &lo);
        ev(I, s->to, &hi);
        if (s->step) ev(I, s->step, &st);
        if (I->signal == SIG_ABORT) break;
        u32 *k = I->frame + s->sym->slot;
        k[0] = lo;
        i32 n = 0;
        bool stuck = false;
        for (;;) {
            bool ok = n < maxit;
            bool c = wi(st) >= 0 ? wi(k[0]) <= wi(hi) : wi(k[0]) >= wi(hi);
            stuck = !ok && c;
            if (!(ok && c)) break;
            n++;
            if (!loop_tick(I)) break;
            exec_block(I, &s->body);
            if (after_body(I)) break;
            k[0] = k[0] + st;
        }
        if (I->signal == SIG_NONE && !I->quiet) { I->iterations = n; I->stuck = stuck; }
        break;
    }
    case S_FOREACH: {
        Type *ct = s->from->type;
        u32 *coll = push_words(I, ct->size);
        ev(I, s->from, coll);
        if (I->signal == SIG_ABORT) break;
        i32 len = ct->kind == T_VEC ? ct->n : wi(coll[0]);
        const u32 *data = ct->kind == T_VEC ? coll : coll + ct->data_off;
        u32 *x = I->frame + s->sym->slot;
        i32 n = 0, j = 0;
        bool stuck = false;
        for (;;) {
            bool ok = n < maxit;
            bool c = j < len;
            stuck = !ok && c;
            if (!(ok && c)) break;
            n++;
            if (!loop_tick(I)) break;
            memcpy(x, data + j * ct->stride, sizeof(u32) * (size_t)ct->elem->size);
            j++;
            exec_block(I, &s->body);
            if (after_body(I)) break;
        }
        if (I->signal == SIG_NONE && !I->quiet) { I->iterations = n; I->stuck = stuck; }
        break;
    }
    case S_BREAK: I->signal = SIG_BREAK; break;
    case S_CONTINUE: I->signal = SIG_CONTINUE; break;
    case S_RETURN: I->signal = SIG_RETURN; break;
    case S_DRAW: {
        if (!I->draws) break;
        DrawItem d = {0};
        d.shape = s->shape;
        d.viewer = s->viewer_index;
        d.color[0] = d.color[1] = d.color[2] = 1.0f;
        d.width = 2.0f;
        d.radius = 4.0f;
        d.radius_px = true;
        u32 v[3];
        if (s->p0) { ev(I, s->p0, v); d.p0[0] = wf(v[0]); d.p0[1] = wf(v[1]); }
        if (s->p1) { ev(I, s->p1, v); d.p1[0] = wf(v[0]); d.p1[1] = wf(v[1]); }
        if (s->radius) { ev(I, s->radius, v); d.radius = wf(v[0]); d.radius_px = s->radius_px; }
        if (s->width) { ev(I, s->width, v); d.width = wf(v[0]); }
        if (s->color) { ev(I, s->color, v); for (int i = 0; i < 3; i++) d.color[i] = wf(v[i]); }
        if (I->signal != SIG_ABORT) vec_push(*I->draws, d);
        break;
    }
    }
    I->sp = mark;
}

static void exec_block(Interp *I, Block *b) {
    for (int i = 0; i < b->n; i++) {
        exec_stmt(I, b->stmts[i]);
        if (I->signal != SIG_NONE) return;
    }
}

/* ---- public API ------------------------------------------------------------ */
#define INTERP_STACK_WORDS (1 << 20)

void interp_init(Interp *I, Program *p, u32 *state) {
    memset(I, 0, sizeof *I);
    I->prog = p;
    I->state = state;
    I->cap = INTERP_STACK_WORDS;
    I->stack = (u32 *)xmalloc(sizeof(u32) * (size_t)I->cap);
    I->step_limit = 2e8;
}

void interp_free(Interp *I) {
    free(I->stack);
    I->stack = NULL;
}

/* Run a section with a fresh frame; `setup` may write the frame first. */
static bool interp_run_section_frame(Interp *I, Section *s, u32 **frame_out) {
    I->sp = 0;
    I->signal = SIG_NONE;
    I->stuck = false;
    I->iterations = 0;
    I->error[0] = 0;
    u32 *frame = push_words(I, s->frame_words);
    I->frame = frame;
    if (frame_out) *frame_out = frame;
    return true;
}

static bool interp_finish(Interp *I) {
    bool ok = I->signal != SIG_ABORT;
    I->signal = SIG_NONE;
    return ok;
}

bool interp_run_section(Interp *I, Section *s) {
    interp_run_section_frame(I, s, NULL);
    exec_block(I, &s->body);
    return interp_finish(I);
}

/* Evaluate a viewer at one point on the CPU (reference implementation). */
bool interp_eval_viewer(Interp *I, Section *v, float zre, float zim, float rgb[3]) {
    u32 *frame;
    interp_run_section_frame(I, v, &frame);
    if (v->coord) {
        frame[v->coord->slot] = fw(zre);
        frame[v->coord->slot + 1] = fw(zim);
    }
    exec_block(I, &v->body);
    for (int k = 0; k < 3; k++) {
        float c = wf(frame[v->color->slot + k]);
        rgb[k] = c != c ? 0.0f : FS_CLAMP(c, 0.0f, 1.0f);
    }
    return interp_finish(I);
}

/* Default state values, before @global/@config run. */
void state_set_defaults(Program *p, u32 *state) {
    memset(state, 0, sizeof(u32) * (size_t)p->state_words);
    state[p->s_max_iter->slot] = 1000;
    state[p->s_epsilon->slot] = fw(1e-6f);
    state[p->s_escape->slot] = fw(2.0f);
    state[p->s_width->slot] = 1024;
    state[p->s_height->slot] = 768;
    for (int v = 0; v < p->nviewers; v++) {
        Section *s = p->viewers[v];
        if (s->radius) state[s->radius->slot] = fw(2.0f);
    }
}

/* Map pixel (px, py) of a w x h tile to the plane, exactly like the shader. */
void viewer_pixel_to_plane(const Program *p, const u32 *state, const Section *v, float w, float h, float px, float py,
                           float *re, float *im) {
    float cx = wf(state[v->center->slot]), cy = wf(state[v->center->slot + 1]);
    float r = wf(state[v->radius->slot]);
    float scale = r / (0.5f * FS_MIN(w, h));
    *re = cx + scale * (px - 0.5f * w);
    *im = cy + scale * (0.5f * h - py);
}
