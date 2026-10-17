/* flood.c - rendering viewers that contain a `continuing` solve.
 *
 * Such a viewer follows a solution (say a critical point found by Newton's
 * method) from pixel to pixel: each pixel starts Newton from the solution of
 * a neighbouring pixel that has already converged, walking the viewer's
 * variable from the neighbour's point to its own in small steps.  Pixels
 * where Newton fails do not pass anything on, so the fill goes around them
 * (as SaddleDrop did).
 *
 * The order is fixed so that the CPU reference and the GPU (compute shaders,
 * codegen_viewer_flood) compute exactly the same thing:
 *   1. rings around the start pixel (the pixel of the point P after
 *      `continuing from c = P`), ring d = pixels at Chebyshev distance d,
 *      outwards; a pixel of ring d continues from a converged neighbour in
 *      ring d-1 or d+1 (never d, so a ring can be computed in parallel);
 *   2. FLOOD_SWEEPS - 1 more sweeps over the rings, alternately inwards and
 *      outwards, for the pixels still waiting (behind failed regions);
 *   3. one last pass over every pixel still waiting: a plain solve from the
 *      seed, without continuation;
 *   4. FLOOD_MEND_ROUNDS rounds of mending (see flood_mend_source): a thin band
 *      of pixels whose solutions jump away from those on both of its sides
 *      (which agree with each other) is solved again, continuing from one
 *      side.  Such bands are left behind when the continuation passes between
 *      two nearby branch points (where the solution meets another one): the
 *      rows passing between them come back with another solution and carry it
 *      to the edge of the picture.  Mending leaves only the short cut between
 *      the branch points.  A thin band between two regions that disagree goes
 *      to the nearer one (one cut instead of two).  Bands lie along the rays
 *      from the start pixel, so they are looked for across the rays.  One pass decides (writes
 *      `pad`), the next solves.
 * Every pass only reads entries that earlier passes wrote. */

enum { FLOOD_PENDING = 0, FLOOD_DONE = 1, FLOOD_FAILED = 2 };
#define FLOOD_SWEEPS 3
#define FLOOD_MEND_ROUNDS 3

/* one entry per pixel; the same layout as the GPU buffer (std430: vec2 + uint + pad) */
typedef struct FloodEntry {
    float t[2];
    u32 status;
    u32 pad;  /* after a decide pass: 0, or 1 + the pixel to continue from when mending this one */
} FloodEntry;

typedef struct FloodPlan {
    int w, h;    /* pixels */
    int sx, sy;  /* start pixel */
    int rings;   /* rings 0 .. rings-1 cover the image */
} FloodPlan;

static int flood_imax(int a, int b) { return a > b ? a : b; }
static int flood_iabs(int a) { return a < 0 ? -a : a; }

void flood_plan_init(FloodPlan *fp, int w, int h, int sx, int sy) {
    fp->w = w;
    fp->h = h;
    fp->sx = FS_CLAMP(sx, 0, w - 1);
    fp->sy = FS_CLAMP(sy, 0, h - 1);
    fp->rings = 1 + flood_imax(flood_imax(fp->sx, w - 1 - fp->sx), flood_imax(fp->sy, h - 1 - fp->sy));
}

int flood_pass_count(const FloodPlan *fp) { return FLOOD_SWEEPS * fp->rings + 1 + 2 * FLOOD_MEND_ROUNDS; }

/* what pass number `index` does */
void flood_pass_info(const FloodPlan *fp, int index, int *kind, int *ring) {
    int sweep = index / fp->rings, k = index % fp->rings;
    if (sweep >= FLOOD_SWEEPS) {
        int r = index - FLOOD_SWEEPS * fp->rings;
        *kind = r == 0 ? FLOOD_PASS_COLD : (r % 2 == 1 ? FLOOD_PASS_DECIDE : FLOOD_PASS_MEND);
        *ring = 0;
        return;
    }
    *kind = FLOOD_PASS_RING;
    *ring = (sweep & 1) ? fp->rings - 1 - k : k; /* odd sweeps go inwards */
}

int flood_ring_size(int ring) { return ring == 0 ? 1 : 8 * ring; }

/* i-th pixel of a ring (clockwise from the top-left corner); false if outside the image */
bool flood_ring_pixel(const FloodPlan *fp, int ring, int i, int *x, int *y) {
    int px, py;
    if (ring == 0) {
        px = fp->sx;
        py = fp->sy;
    } else {
        int side = i / (2 * ring), j = i % (2 * ring);
        switch (side) {
        case 0: px = fp->sx - ring + j; py = fp->sy - ring; break;
        case 1: px = fp->sx + ring; py = fp->sy - ring + j; break;
        case 2: px = fp->sx + ring - j; py = fp->sy + ring; break;
        default: px = fp->sx - ring; py = fp->sy + ring - j; break;
        }
    }
    *x = px;
    *y = py;
    return px >= 0 && py >= 0 && px < fp->w && py < fp->h;
}

int flood_ring_of(const FloodPlan *fp, int x, int y) { return flood_imax(flood_iabs(x - fp->sx), flood_iabs(y - fp->sy)); }

/* The converged neighbour (x, y) continues from: in ring d-1 or d+1, preferring d-1, then the one
 * closest to the line from the start pixel through (x, y).  Returns its index or -1.  codegen.c
 * emits the same computation for the GPU (keep them in sync). */
static const int flood_dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
static const int flood_dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

int flood_pick_neighbour(const FloodPlan *fp, const FloodEntry *field, int x, int y) {
    int d = flood_ring_of(fp, x, y), best = -1, best_score = 0x7fffffff;
    for (int k = 0; k < 8; k++) {
        int nx = x + flood_dx[k], ny = y + flood_dy[k];
        if (nx < 0 || ny < 0 || nx >= fp->w || ny >= fp->h) continue;
        int dn = flood_ring_of(fp, nx, ny);
        if (dn == d) continue;
        if (field[ny * fp->w + nx].status != FLOOD_DONE) continue;
        int cross = flood_iabs((nx - fp->sx) * (y - fp->sy) - (ny - fp->sy) * (x - fp->sx));
        int score = (dn < d ? 0 : 1 << 20) + cross;
        if (score < best_score) {
            best_score = score;
            best = ny * fp->w + nx;
        }
    }
    return best;
}

/* ---- mending --------------------------------------------------------------------- */
/* Thickest band mended: about 1/40 of the picture (bands are features of the plane, so they get
 * thicker in larger pictures), at least 8 pixels.  codegen.c computes the same. */
int flood_mend_limit(int w, int h) { return FS_CLAMP(FS_MIN(w, h) / 40, 8, FLOOD_MEND_KMAX); }

static float flood_cabs(float re, float im) { return sqrtf(re * re + im * im); }
#define FLOOD_FAR 1e30f /* "no difference available" */

/* Along the line through pixel (x, y) in direction (ux, uy): the solutions of the pixels
 * j = -(n) .. n steps away (done[] false where not converged or outside). */
typedef struct FloodLine {
    bool done[2 * FLOOD_MEND_KMAX + 3];
    float re[2 * FLOOD_MEND_KMAX + 3], im[2 * FLOOD_MEND_KMAX + 3];
} FloodLine;
#define FLOOD_LINE_AT(j) ((j) + FLOOD_MEND_KMAX + 1)

static float flood_line_diff(const FloodLine *L, int a, int b) { /* |w_b - w_a| or FLOOD_FAR */
    if (!L->done[FLOOD_LINE_AT(a)] || !L->done[FLOOD_LINE_AT(b)]) return FLOOD_FAR;
    return flood_cabs(L->re[FLOOD_LINE_AT(b)] - L->re[FLOOD_LINE_AT(a)], L->im[FLOOD_LINE_AT(b)] - L->im[FLOOD_LINE_AT(a)]);
}

/* Is there a jump between the pixels a and b = a + s (s = +-1) of the line: a difference much
 * larger than the differences next to it (in a smooth region they are all about the same)? */
static bool flood_line_jump(const FloodLine *L, int a, int s) {
    int b = a + s;
    float d = flood_line_diff(L, a, b);
    if (d >= FLOOD_FAR) return false;
    float g = FS_MIN(flood_line_diff(L, a - s, a), flood_line_diff(L, b, b + s));
    if (g >= FLOOD_FAR) return false;
    float size = flood_cabs(L->re[FLOOD_LINE_AT(a)], L->im[FLOOD_LINE_AT(a)]) + flood_cabs(L->re[FLOOD_LINE_AT(b)], L->im[FLOOD_LINE_AT(b)]);
    return d > 8.0f * g + 1e-4f * size;
}

/* The first FLOOD_MEND_JUMPS jumps from pixel 0 going in direction s: the k such that (s(k-1), sk)
 * jumps, within kmax steps and before a pixel that did not converge.  Returns how many. */
static int flood_line_jumps(const FloodLine *L, int s, int kmax, int ks[FLOOD_MEND_JUMPS]) {
    int n = 0;
    for (int k = 1; k <= kmax && n < FLOOD_MEND_JUMPS; k++) {
        if (!L->done[FLOOD_LINE_AT(s * k)]) break;
        if (flood_line_jump(L, s * (k - 1), s)) ks[n++] = k;
    }
    return n;
}

/* Do the pixels a > 0 and b < 0 of the line agree, allowing for the variation between pixels just
 * beyond them (they are t + 1 pixels apart)?  The smaller variation counts: the larger one may be
 * a jump to yet another band. */
static bool flood_line_agree(const FloodLine *L, int a, int b, int t) {
    float g = FS_MIN(flood_line_diff(L, a, a + 1), flood_line_diff(L, b, b - 1));
    if (g >= FLOOD_FAR) return false;
    float d = flood_line_diff(L, a, b);
    float size = flood_cabs(L->re[FLOOD_LINE_AT(a)], L->im[FLOOD_LINE_AT(a)]) + flood_cabs(L->re[FLOOD_LINE_AT(b)], L->im[FLOOD_LINE_AT(b)]);
    return d <= 4.0f * (float)(t + 1) * g + 1e-4f * size;
}

/* Is the pixel beyond the jump at k (direction s) clean: does the next pixel outwards continue it
 * smoothly (rather than being more debris)? */
static bool flood_line_clean(const FloodLine *L, int s, int k) {
    float out = flood_line_diff(L, s * k, s * (k + 1));
    return out < FLOOD_FAR && 8.0f * out <= flood_line_diff(L, s * (k - 1), s * k);
}

/* Pixel (x, y) (converged) is in a band across the axis (ux, uy) when there is a jump on each side,
 * at most kmax pixels apart, with agreeing pixels beyond them (bands may hold several solutions,
 * so the first few jumps on each side are tried, the thinnest band wins).  Failing that, a band
 * between two regions that do not agree (the continuation found a third solution between them)
 * goes to the nearer region, so that one cut is left instead of two.  Returns 0 (no band), the
 * band's thickness, or (the second kind) its thickness + FLOOD_MEND_KMAX + 1; and in *src, the
 * pixel beyond the nearer of its two jumps. */
static int flood_mend_axis(const FloodPlan *fp, const FloodEntry *field, int x, int y, int ux, int uy, int kmax, int *src) {
    FloodLine L;
    for (int j = -(kmax + 1); j <= kmax + 1; j++) {
        int px = x + j * ux, py = y + j * uy, at = FLOOD_LINE_AT(j);
        bool in = px >= 0 && py >= 0 && px < fp->w && py < fp->h;
        const FloodEntry *e = in ? &field[py * fp->w + px] : NULL;
        L.done[at] = e && e->status == FLOOD_DONE;
        L.re[at] = e ? e->t[0] : 0;
        L.im[at] = e ? e->t[1] : 0;
    }
    int kp[FLOOD_MEND_JUMPS], km[FLOOD_MEND_JUMPS];
    int np = flood_line_jumps(&L, 1, kmax, kp), nm = np ? flood_line_jumps(&L, -1, kmax, km) : 0;
    int best = 0, bestk = 0;
    for (int i = 0; i < np; i++)
        for (int j = 0; j < nm; j++) {
            int t = kp[i] + km[j] - 1;
            if (t > kmax || (best && t >= best) || !flood_line_agree(&L, kp[i], -km[j], t)) continue;
            best = t;
            bestk = kp[i] <= km[j] ? kp[i] : -km[j];
        }
    if (!best) { /* between regions that disagree: the nearest clean region on each side */
        int cp = 0, cm = 0;
        for (int i = np - 1; i >= 0; i--)
            if (flood_line_clean(&L, 1, kp[i])) cp = kp[i];
        for (int j = nm - 1; j >= 0; j--)
            if (flood_line_clean(&L, -1, km[j])) cm = km[j];
        if (cp && cm && cp + cm - 1 <= kmax) {
            best = cp + cm - 1 + FLOOD_MEND_KMAX + 1;
            bestk = cp <= cm ? cp : -cm;
        }
    }
    if (best) *src = (y + bestk * uy) * fp->w + (x + bestk * ux);
    return best;
}

/* The decision of a decide pass for pixel (x, y): 0 (keep it) or 1 + the pixel to continue from.
 * Solutions travel along the rays from the start pixel, and so do bands: they are crossed along the
 * axis closer to perpendicular to the ray (vertically left and right of the start pixel). */
u32 flood_mend_source(const FloodPlan *fp, const FloodEntry *field, int x, int y) {
    if (field[y * fp->w + x].status != FLOOD_DONE) return 0;
    int kmax = flood_mend_limit(fp->w, fp->h), src = -1;
    bool vertical = flood_iabs(x - fp->sx) >= flood_iabs(y - fp->sy);
    return flood_mend_axis(fp, field, x, y, vertical ? 0 : 1, vertical ? 1 : 0, kmax, &src) ? (u32)src + 1 : 0;
}

/* start pixel of a viewer: where the point after `continuing from` is (clamped to the image) */
bool flood_start_pixel(Program *p, const u32 *state, Section *v, int w, int h, int *sx, int *sy) {
    u32 *st = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(st, state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, st);
    u32 val[2] = {0, 0};
    bool ok = interp_eval_global_expr(&I, v->flood_start, val);
    interp_free(&I);
    free(st);
    float re = wf(val[0]), im = wf(val[1]);
    float cx = wf(state[v->center->slot]), cy = wf(state[v->center->slot + 1]);
    float scale = wf(state[v->radius->slot]) / (0.5f * FS_MIN((float)w, (float)h));
    float px = 0.5f * w + (re - cx) / scale, py = 0.5f * h - (im - cy) / scale;
    if (!(px == px) || !(py == py)) px = 0.5f * w, py = 0.5f * h;
    px = FS_CLAMP(px, 0.0f, (float)w - 1);
    py = FS_CLAMP(py, 0.0f, (float)h - 1);
    *sx = (int)px;
    *sy = (int)py;
    return ok;
}

/* run one pixel on the CPU; returns false when the pixel is not ready (no converged neighbour) */
static bool flood_cpu_pixel(Program *p, const u32 *state, u32 *scratch, Section *v, const FloodPlan *fp,
                            FloodEntry *field, u8 *rgba, int x, int y, int kind) {
    FloodEntry *e = &field[y * fp->w + x];
    int mode = FLOOD_DIRECT, nb = -1;
    if (kind == FLOOD_PASS_MEND) {
        if (e->status != FLOOD_DONE || !e->pad) return true;
        /* the source may be mended in this pass too: then continue from where it would (a few hops) */
        nb = (int)e->pad - 1;
        for (int hop = 0; hop < FLOOD_MEND_HOPS && field[nb].pad; hop++) nb = (int)field[nb].pad - 1;
        if (field[nb].pad || field[nb].status != FLOOD_DONE) return true;
        mode = FLOOD_CONTINUE;
    } else if (e->status != FLOOD_PENDING) {
        return true;
    } else if (kind == FLOOD_PASS_COLD) {
        mode = FLOOD_DIRECT;
    } else {
        nb = flood_pick_neighbour(fp, field, x, y);
        if (nb >= 0) mode = FLOOD_CONTINUE;
        else if (x == fp->sx && y == fp->sy) mode = FLOOD_START;
        else return false;
    }
    memcpy(scratch, state, sizeof(u32) * (size_t)p->state_words);
    Interp I;
    interp_init(&I, p, scratch);
    I.flood_mode = mode;
    if (nb >= 0) {
        int nx = nb % fp->w, ny = nb / fp->w;
        I.flood_seed[0] = field[nb].t[0];
        I.flood_seed[1] = field[nb].t[1];
        viewer_pixel_to_plane(p, scratch, v, (float)fp->w, (float)fp->h, nx + 0.5f, ny + 0.5f, &I.flood_from[0], &I.flood_from[1]);
    }
    I.flood_out_ok = -1;
    float re, im, c[3];
    viewer_pixel_to_plane(p, scratch, v, (float)fp->w, (float)fp->h, x + 0.5f, y + 0.5f, &re, &im);
    I.pixel = viewer_pixel_size(p, scratch, v, (float)fp->w, (float)fp->h, x + 0.5f, y + 0.5f);
    interp_eval_viewer(&I, v, re, im, c);
    interp_free(&I);
    if (kind == FLOOD_PASS_MEND && I.flood_out_ok != 1) return true; /* keep what it had */
    e->t[0] = I.flood_out[0];
    e->t[1] = I.flood_out[1];
    e->status = I.flood_out_ok == 1 ? FLOOD_DONE : FLOOD_FAILED;
    u8 *px = rgba + 4 * ((size_t)y * fp->w + x);
    for (int k = 0; k < 3; k++) px[k] = (u8)(c[k] * 255.0f + 0.5f);
    px[3] = 255;
    return true;
}

/* CPU reference rendering of a flood viewer (rows top to bottom, RGBA8). `field_out` may be NULL. */
void flood_render_cpu(Program *p, const u32 *state, Section *v, int w, int h, u8 *rgba, FloodEntry *field_out) {
    FloodPlan fp;
    int sx, sy;
    flood_start_pixel(p, state, v, w, h, &sx, &sy);
    flood_plan_init(&fp, w, h, sx, sy);
    FloodEntry *field = field_out ? field_out : (FloodEntry *)xmalloc(sizeof(FloodEntry) * (size_t)w * h);
    memset(field, 0, sizeof(FloodEntry) * (size_t)w * h);
    memset(rgba, 0, (size_t)w * h * 4);
    u32 *scratch = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    int npass = flood_pass_count(&fp);
    for (int pass = 0; pass < npass; pass++) {
        int kind, ring;
        flood_pass_info(&fp, pass, &kind, &ring);
        if (kind == FLOOD_PASS_DECIDE) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) field[y * w + x].pad = flood_mend_source(&fp, field, x, y);
        } else if (kind != FLOOD_PASS_RING) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) flood_cpu_pixel(p, state, scratch, v, &fp, field, rgba, x, y, kind);
        } else {
            /* a ring only reads rings d-1 and d+1, so the order inside the ring doesn't matter */
            int n = flood_ring_size(ring);
            for (int i = 0; i < n; i++) {
                int x, y;
                if (flood_ring_pixel(&fp, ring, i, &x, &y)) flood_cpu_pixel(p, state, scratch, v, &fp, field, rgba, x, y, kind);
            }
        }
    }
    free(scratch);
    if (!field_out) free(field);
}

/* ---- GPU ------------------------------------------------------------------------- */
/* One flood viewer on the GPU: the compute pipeline (codegen_viewer_flood), the field buffer
 * (FloodEntry per pixel) and the colour buffer (packed RGBA8 per pixel), and how far the fill got. */
typedef struct FloodGpu {
    int pipe;
    int field, colors;
    int w, h;
    FloodPlan plan;
    int next_pass;  /* next pass to record; == pass count when finished */
    bool restart;   /* clear the buffers before the next pass */
} FloodGpu;

typedef struct FloodPush { float w, h; i32 sx, sy; i32 ring, mode; } FloodPush; /* matches codegen */

/* the compute pipeline of a flood viewer; with `df`, in double-single precision (deep zooms) */
bool flood_gpu_create_ex(FloodGpu *fg, Program *p, Section *v, bool df, char *err, int errcap) {
    memset(fg, 0, sizeof *fg);
    u32 n;
    u32 *words = codegen_viewer_flood_ex(p, v, df, &n, err, errcap);
    if (!words) return false;
    GfxComputeDesc d = {0};
    d.spirv = words;
    d.nwords = n;
    d.nbuffers = 2;
    d.push_bytes = sizeof(FloodPush);
    fg->pipe = gfx_compute_create(&d, err, errcap);
    free(words);
    return fg->pipe > 0;
}
bool flood_gpu_create(FloodGpu *fg, Program *p, Section *v, char *err, int errcap) {
    return flood_gpu_create_ex(fg, p, v, false, err, errcap);
}

void flood_gpu_destroy(FloodGpu *fg) {
    if (fg->pipe) gfx_compute_destroy(fg->pipe);
    if (fg->field) gfx_buffer_destroy(fg->field);
    if (fg->colors) gfx_buffer_destroy(fg->colors);
    memset(fg, 0, sizeof *fg);
}

/* (re)start the fill for a w x h image from pixel (sx, sy) */
bool flood_gpu_start(FloodGpu *fg, int w, int h, int sx, int sy) {
    if (w != fg->w || h != fg->h || !fg->field) {
        if (fg->field) gfx_buffer_destroy(fg->field);
        if (fg->colors) gfx_buffer_destroy(fg->colors);
        fg->field = gfx_buffer_create((u64)w * h * sizeof(FloodEntry));
        fg->colors = gfx_buffer_create((u64)w * h * 4);
        fg->w = w;
        fg->h = h;
        if (!fg->field || !fg->colors) return false;
    }
    flood_plan_init(&fg->plan, w, h, sx, sy);
    fg->next_pass = 0;
    fg->restart = true;
    return true;
}

bool flood_gpu_done(const FloodGpu *fg) { return !fg->field || fg->next_pass >= flood_pass_count(&fg->plan); }

/* give the buffers back (the pipeline stays; flood_gpu_start makes them again) */
void flood_gpu_free_buffers(FloodGpu *fg) {
    if (fg->field) gfx_buffer_destroy(fg->field);
    if (fg->colors) gfx_buffer_destroy(fg->colors);
    fg->field = fg->colors = 0;
    fg->w = fg->h = 0;
}

/* Record up to `max_passes` passes (and the copy of the colours into `target`, whose picture is kept
 * where the fill hasn't arrived yet).  Call between gfx_frame_begin and gfx_frame_draw, or in a batch. */
void flood_gpu_record(FloodGpu *fg, int max_passes, int target) {
    if (!fg->field || flood_gpu_done(fg)) return;
    if (fg->restart) {
        gfx_frame_fill_buffer(fg->field, 0, (u64)fg->w * fg->h * sizeof(FloodEntry), 0);
        if (target) gfx_frame_target_to_buffer(target, fg->colors, 0);
        fg->restart = false;
    }
    int npass = flood_pass_count(&fg->plan);
    int bufs[2] = {fg->field, fg->colors};
    for (int k = 0; k < max_passes && fg->next_pass < npass; k++, fg->next_pass++) {
        int kind, ring;
        flood_pass_info(&fg->plan, fg->next_pass, &kind, &ring);
        FloodPush pc = {(float)fg->w, (float)fg->h, fg->plan.sx, fg->plan.sy, ring, kind};
        u32 count = kind != FLOOD_PASS_RING ? (u32)(fg->w * fg->h) : (u32)flood_ring_size(ring);
        gfx_frame_dispatch(fg->pipe, bufs, &pc, (count + FLOOD_GROUP_SIZE - 1) / FLOOD_GROUP_SIZE, 1, 1);
    }
    if (target) gfx_frame_buffer_to_target(fg->colors, 0, target);
}

/* Headless: render a flood viewer into memory (rgba8, rows top to bottom). The globals must be set. */
bool flood_render_gpu_ex(Program *p, const u32 *state, Section *v, bool df, int w, int h, u8 *rgba, FloodEntry *field_out,
                         char *err, int errcap) {
    FloodGpu fg;
    if (!flood_gpu_create_ex(&fg, p, v, df, err, errcap)) return false;
    int sx, sy;
    flood_start_pixel(p, state, v, w, h, &sx, &sy);
    int target = gfx_target_create(w, h);
    bool ok = target > 0 && flood_gpu_start(&fg, w, h, sx, sy);
    if (!ok) snprintf(err, (size_t)errcap, "could not allocate the GPU buffers for a %dx%d flood fill", w, h);
    while (ok && !flood_gpu_done(&fg)) { /* a few hundred passes per submission (GPU watchdogs) */
        if (!gfx_batch_begin()) {
            snprintf(err, (size_t)errcap, "could not record GPU work");
            ok = false;
            break;
        }
        flood_gpu_record(&fg, 256, target);
        ok = gfx_batch_end(err, errcap);
    }
    if (ok) ok = gfx_target_read(target, rgba, err, errcap);
    if (ok && field_out) ok = gfx_buffer_download(fg.field, 0, field_out, (u64)w * h * sizeof(FloodEntry), err, errcap);
    if (target > 0) gfx_target_destroy(target);
    flood_gpu_destroy(&fg);
    return ok;
}

bool flood_render_gpu(Program *p, const u32 *state, Section *v, int w, int h, u8 *rgba, FloodEntry *field_out, char *err,
                      int errcap) {
    return flood_render_gpu_ex(p, state, v, false, w, h, rgba, field_out, err, errcap);
}
