/* video.c - videos of a view: its frames over time (the .time of the state), rendered offscreen and antialiased like
 * a view at rest, written as H.264 in an MP4 file.  The encoder and the MP4 writer are third_party/minih264 and
 * third_party/minimp4 (public domain), compiled on their own (see build.sh).  The app (Save a video) and the command
 * line (--video) both make their videos with a VideoJob. */
#include "minih264e.h"
#include "minimp4.h"

bool app_init_exact_floats(void);

/* where the k-th sample of a pixel lies, from its center (k = 0: the center): points of the R2 sequence, which spread
   evenly over the pixel from the first ones on, in pairs q, -q (samples 1 and 2, 3 and 4...) so that the samples'
   average stays at the center and the picture doesn't shift as they come in */
static void aa_offset(int k, float *dx, float *dy) {
    *dx = *dy = 0;
    if (k <= 0) return;
    int j = (k + 1) / 2;
    double x = 0.5 + j * 0.7548776662466927, y = 0.5 + j * 0.5698402909980532;
    float s = k % 2 ? 1.0f : -1.0f;
    *dx = s * (float)(x - floor(x) - 0.5);
    *dy = s * (float)(y - floor(y) - 0.5);
}

/* ---- the file: H.264 frames in an MP4 container -------------------------------------------------------------------- */
#define VIDEO_QP 23          /* the encoder's quantizer, the same for every frame: 10 (best) to 51 (worst) */
#define VIDEO_MAX_SIDE 4096  /* (H.264 level 5.1: about 4K) */

typedef struct VideoFile {
    FILE *f;
    MP4E_mux_t *mux;
    mp4_h26x_writer_t wr;
    bool wr_open;
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    u8 *yuv;          /* the frame in 4:2:0: Y (h rows of ys bytes), then U and V (h/2 rows of cs bytes each) */
    int w, h, ys, cs, fps, frames;
    bool write_failed;
} VideoFile;

static int video_write(int64_t offset, const void *buf, size_t size, void *token) {
    VideoFile *vf = (VideoFile *)token;
#if FS_OS_WINDOWS
    int sought = _fseeki64(vf->f, (__int64)offset, SEEK_SET);
#else
    int sought = fseeko(vf->f, (off_t)offset, SEEK_SET);
#endif
    if (sought != 0 || fwrite(buf, 1, size, vf->f) != size) vf->write_failed = true;
    return vf->write_failed;
}

static void video_free(VideoFile *vf) {
    if (!vf) return;
    if (vf->f) fclose(vf->f);
    free(vf->enc);
    free(vf->scratch);
    free(vf->yuv);
    free(vf);
}

/* A new video file: w x h pixels (even numbers, at most VIDEO_MAX_SIDE), fps frames per second. */
static VideoFile *video_file_create(const char *path, int w, int h, int fps, char *err, int errcap) {
    if (w < 2 || h < 2 || (w | h) & 1 || w > VIDEO_MAX_SIDE || h > VIDEO_MAX_SIDE || fps < 1 || fps > 240) {
        snprintf(err, (size_t)errcap, "a video can't be %dx%d at %d frames per second", w, h, fps);
        return NULL;
    }
    VideoFile *vf = (VideoFile *)xcalloc(1, sizeof *vf);
    vf->w = w;
    vf->h = h;
    vf->fps = fps;
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = w;
    cp.height = h;
    cp.gop = 2 * fps;          /* a key frame every 2 seconds (where players can jump to) */
    cp.const_input_flag = 1;   /* (the encoder leaves the frame alone: then any even size works) */
    int sp = 0, ss = 0;
    if (H264E_sizeof(&cp, &sp, &ss) != H264E_STATUS_SUCCESS || sp <= 0 || ss <= 0) {
        snprintf(err, (size_t)errcap, "the video encoder doesn't take %dx%d", w, h);
        video_free(vf);
        return NULL;
    }
    vf->enc = (H264E_persist_t *)xmalloc((size_t)sp);
    vf->scratch = (H264E_scratch_t *)xmalloc((size_t)ss);
    if (H264E_init(vf->enc, &cp) != H264E_STATUS_SUCCESS) {
        snprintf(err, (size_t)errcap, "the video encoder doesn't start");
        video_free(vf);
        return NULL;
    }
    vf->ys = (w + 15) & ~15; /* (rows of multiples of 16 bytes: the encoder's SIMD code wants 8) */
    vf->cs = (w / 2 + 15) & ~15;
    vf->yuv = (u8 *)xcalloc((size_t)vf->ys * h + (size_t)vf->cs * h, 1);
    vf->f = fopen(path, "wb");
    if (!vf->f) {
        snprintf(err, (size_t)errcap, "can't write %.400s", path);
        video_free(vf);
        return NULL;
    }
    vf->mux = MP4E_open(0, 0, vf, video_write);
    if (!vf->mux || mp4_h26x_write_init(&vf->wr, vf->mux, w, h, 0) != MP4E_STATUS_OK) {
        snprintf(err, (size_t)errcap, "can't start the video file %.400s", path);
        if (vf->mux) MP4E_close(vf->mux);
        video_free(vf);
        return NULL;
    }
    vf->wr_open = true;
    return vf;
}

/* RGBA (rows from the top) to 4:2:0 Y'CbCr, BT.709, video range (what the file says, see third_party/minih264);
   rows of ys bytes in Y, cs in U and V */
static void video_rgba_to_yuv(const u8 *rgba, int w, int h, u8 *Y, int ys, u8 *U, u8 *V, int cs) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const u8 *c = rgba + 4 * ((size_t)y * w + x);
            Y[(size_t)y * ys + x] = (u8)(16 + ((47 * c[0] + 157 * c[1] + 16 * c[2] + 128) >> 8));
        }
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            const u8 *a = rgba + 4 * ((size_t)(2 * y) * w + 2 * x), *b = a + 4 * (size_t)w;
            int r = a[0] + a[4] + b[0] + b[4], g = a[1] + a[5] + b[1] + b[5], bl = a[2] + a[6] + b[2] + b[6];
            U[(size_t)y * cs + x] = (u8)FS_CLAMP(128 + ((-26 * r - 87 * g + 113 * bl + 512) >> 10), 0, 255);
            V[(size_t)y * cs + x] = (u8)FS_CLAMP(128 + ((112 * r - 102 * g - 10 * bl + 512) >> 10), 0, 255);
        }
}

/* the next frame (RGBA, w x h) */
static bool video_file_add(VideoFile *vf, const u8 *rgba, char *err, int errcap) {
    int w = vf->w, h = vf->h;
    u8 *Y = vf->yuv, *U = Y + (size_t)vf->ys * h, *V = U + (size_t)vf->cs * (h / 2);
    video_rgba_to_yuv(rgba, w, h, Y, vf->ys, U, V, vf->cs);
    H264E_io_yuv_t io;
    io.yuv[0] = Y;
    io.yuv[1] = U;
    io.yuv[2] = V;
    io.stride[0] = vf->ys;
    io.stride[1] = io.stride[2] = vf->cs;
    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    rp.encode_speed = 4;
    rp.qp_min = rp.qp_max = VIDEO_QP;
    unsigned char *coded = NULL;
    int size = 0;
    if (H264E_encode(vf->enc, vf->scratch, &rp, &io, &coded, &size) != H264E_STATUS_SUCCESS) {
        snprintf(err, (size_t)errcap, "the video encoder failed at frame %d", vf->frames + 1);
        return false;
    }
    if (mp4_h26x_write_nal(&vf->wr, coded, size, (unsigned)(90000 / vf->fps)) != MP4E_STATUS_OK || vf->write_failed) {
        snprintf(err, (size_t)errcap, "can't write frame %d of the video (is the disk full?)", vf->frames + 1);
        return false;
    }
    vf->frames++;
    return true;
}

/* finish the file (written: true) or drop it (written: false, and the file is removed) */
static bool video_file_close(VideoFile *vf, const char *path, bool written, char *err, int errcap) {
    bool ok = written;
    if (vf->mux && MP4E_close(vf->mux) != MP4E_STATUS_OK) ok = false;
    if (vf->wr_open) mp4_h26x_write_close(&vf->wr);
    if (vf->write_failed) ok = false;
    if (vf->f && fclose(vf->f) != 0) ok = false;
    vf->f = NULL;
    if (written && !ok) snprintf(err, (size_t)errcap, "can't finish the video %.400s (is the disk full?)", path);
    if (!ok) remove(path);
    video_free(vf);
    return ok;
}

/* ---- the frames of a view -------------------------------------------------------------------------------------- */
typedef struct VideoJob {
    Program *p;
    Section *v;
    u32 *state;           /* the video's own copy of the state: its .time is the frame's */
    int w, h, fps, samples, frames, done;
    double t0;            /* the time of the first frame */
    int pipe, pipe_df;    /* the viewer's pipelines (single precision, and double-single when a frame needs it) */
    int target;
    u8 *rgba;
    VideoFile *file;
    char path[1024];
    char err[512];
} VideoJob;

/* make the pipeline of viewer v (df: in double-single precision) */
static int video_pipeline(Program *p, Section *v, bool df, char *err, int errcap) {
    u32 n;
    u32 *words = df ? codegen_viewer_ex(p, v, true, &n, err, errcap) : codegen_viewer(p, v, &n, err, errcap);
    int pipe = words ? gfx_viewer_create(words, n, err, errcap) : 0;
    free(words);
    return pipe;
}

static void video_end(VideoJob *j);

/* A video of viewer v of program p: `seconds` long from time t0, w x h (even; at most VIDEO_MAX_SIDE), fps frames per
   second, `samples` samples per pixel (1 to 16; views that follow a solution from pixel to pixel get one), the rest
   of the state as in `state`.  The GPU must be initialized.  NULL (and err) if it can't start. */
static VideoJob *video_begin(Program *p, const u32 *state, Section *v, const char *path, int w, int h, int fps,
                             double seconds, double t0, int samples, char *err, int errcap) {
    int frames = (int)floor(seconds * fps + 0.5);
    if (frames < 1) {
        snprintf(err, (size_t)errcap, "the video needs at least one frame");
        return NULL;
    }
    VideoJob *j = (VideoJob *)xcalloc(1, sizeof *j);
    j->p = p;
    j->v = v;
    j->w = w;
    j->h = h;
    j->fps = fps;
    j->samples = v->flood ? 1 : FS_CLAMP(samples, 1, 16);
    j->frames = frames;
    j->t0 = t0;
    snprintf(j->path, sizeof j->path, "%s", path);
    j->state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(j->state, state, sizeof(u32) * (size_t)p->state_words);
    j->rgba = (u8 *)xmalloc((size_t)w * h * 4);
    bool ok = v->flood || ((j->pipe = video_pipeline(p, v, false, err, errcap)) > 0 && (j->target = gfx_target_create(w, h)) > 0);
    if (ok && !v->flood && !j->target) {
        snprintf(err, (size_t)errcap, "not enough GPU memory for a %dx%d video", w, h);
        ok = false;
    }
    if (ok) ok = (j->file = video_file_create(path, w, h, fps, err, errcap)) != NULL;
    if (!ok) {
        video_end(j);
        return NULL;
    }
    return j;
}

/* render the next frame and add it to the file: 1 when there are more, 0 when the video is finished and written,
   -1 when it failed (j->err; the file is removed) */
static int video_step(VideoJob *j) {
    if (!j->file) return -1;
    Program *p = j->p;
    Section *v = j->v;
    double t = j->t0 + (double)j->done / j->fps;
    j->state[p->s_time->slot] = fw((float)t);
    char *err = j->err;
    int errcap = (int)sizeof j->err;
    bool ok = !p->prepare_reads_time || program_prepare(p, j->state, err, errcap); /* (it may move the view) */
    if (ok) gfx_set_globals(j->state, (u32)p->state_words * 4);
    bool df = ok && viewer_wants_df(p, j->state, v, j->w, j->h, j->pipe_df > 0);
    if (ok && v->flood) {
        ok = flood_render_gpu_ex(p, j->state, v, df, j->w, j->h, j->rgba, NULL, err, errcap);
    } else if (ok) {
        if (df && !j->pipe_df) {
            static bool exact_floats;
            if (!exact_floats) app_init_exact_floats(); /* (once: how deep zooms keep their arithmetic exact) */
            exact_floats = true;
            j->pipe_df = video_pipeline(p, v, true, err, errcap);
            if (!j->pipe_df) j->pipe_df = -1; /* (the GPU doesn't take it: single precision) */
        }
        int pipe = df && j->pipe_df > 0 ? j->pipe_df : j->pipe;
        for (int k = 0; k < j->samples && ok; k++) { /* the samples of each pixel, averaged as in a view at rest */
            GfxViewerParams vp;
            memset(&vp, 0, sizeof vp);
            vp.width = (float)j->w;
            vp.height = (float)j->h;
            aa_offset(k, &vp.offset_x, &vp.offset_y);
            vp.blend = k ? 1.0f / (float)(k + 1) : 0.0f;
            for (int y = 0; y < j->h && ok; y += 256) { /* (in bands: no GPU watchdog timeouts on slow shaders) */
                vp.x0 = 0;
                vp.x1 = j->w;
                vp.y0 = y;
                vp.y1 = FS_MIN(j->h, y + 256);
                ok = gfx_target_render(j->target, pipe, &vp, err, errcap);
            }
        }
        if (ok) ok = gfx_target_read(j->target, j->rgba, err, errcap);
    }
    if (ok) ok = video_file_add(j->file, j->rgba, err, errcap);
    if (!ok) {
        video_file_close(j->file, j->path, false, err, errcap);
        j->file = NULL;
        return -1;
    }
    if (++j->done < j->frames) return 1;
    ok = video_file_close(j->file, j->path, true, err, errcap);
    j->file = NULL;
    return ok ? 0 : -1;
}

/* free the job (an unfinished video is removed) */
static void video_end(VideoJob *j) {
    if (!j) return;
    char err[16];
    if (j->file) video_file_close(j->file, j->path, false, err, sizeof err);
    if (j->pipe > 0) gfx_viewer_destroy(j->pipe);
    if (j->pipe_df > 0) gfx_viewer_destroy(j->pipe_df);
    if (j->target > 0) gfx_target_destroy(j->target);
    free(j->state);
    free(j->rgba);
    free(j);
}
