/* test_video.c - videos (src/video.c): the MP4 file, checked by reading its boxes (no decoder needed), the colors, and
 * a video of a script on the GPU (skipped without a Vulkan device). */

/* the payload of the first box `type` inside [p, end), looking into the boxes that hold others */
static const u8 *mp4_find(const u8 *p, const u8 *end, const char *type, u32 *size) {
    static const char *containers[] = {"moov", "trak", "mdia", "minf", "stbl"};
    while (end - p >= 8) {
        u32 n = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        if (n < 8 || n > (u32)(end - p)) return NULL;
        if (!memcmp(p + 4, type, 4)) {
            *size = n - 8;
            return p + 8;
        }
        for (int k = 0; k < ARRAY_COUNT(containers); k++)
            if (!memcmp(p + 4, containers[k], 4)) {
                const u8 *r = mp4_find(p + 8, p + n, type, size);
                if (r) return r;
            }
        p += n;
    }
    return NULL;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* the file at path: an MP4 with one H.264 track of w x h, `frames` frames, `seconds` long */
static void check_mp4(const char *path, int w, int h, int frames, double seconds) {
    size_t len = 0;
    u8 *f = (u8 *)fs_read_file(path, &len);
    CHECK_MSG(f != NULL, "can't read %s", path);
    if (!f) return;
    const u8 *end = f + len;
    u32 n;
    CHECK(len > 8 && !memcmp(f + 4, "ftyp", 4));
    CHECK(mp4_find(f, end, "mdat", &n) != NULL);
    const u8 *stsz = mp4_find(f, end, "stsz", &n); /* version, flags, sample size, sample count */
    CHECK(stsz && n >= 12);
    if (stsz && n >= 12) CHECK_INT(be32(stsz + 8), frames);
    const u8 *tkhd = mp4_find(f, end, "tkhd", &n); /* (width and height: the last 8 bytes, 16.16) */
    CHECK(tkhd && n >= 8);
    if (tkhd && n >= 8) {
        CHECK_INT(be32(tkhd + n - 8) >> 16, w);
        CHECK_INT(be32(tkhd + n - 4) >> 16, h);
    }
    const u8 *mdhd = mp4_find(f, end, "mdhd", &n); /* version 0: times, timescale, duration */
    CHECK(mdhd && n >= 20 && mdhd[0] == 0);
    if (mdhd && n >= 20 && mdhd[0] == 0) CHECK_NEAR((double)be32(mdhd + 16) / be32(mdhd + 12), seconds, 1e-3);
    bool avcc = false; /* the H.264 settings (in the sample description) */
    for (size_t k = 0; k + 4 <= len && !avcc; k++) avcc = !memcmp(f + k, "avcC", 4);
    CHECK(avcc);
    free(f);
}

TEST(file_boxes) {
    const char *path = "build/test_video.mp4";
    enum { W = 64, H = 48, N = 5 };
    char err[256] = "";
    VideoFile *vf = video_file_create(path, W, H, 10, err, sizeof err);
    CHECK_MSG(vf != NULL, "%s", err);
    if (!vf) return;
    u8 *rgba = (u8 *)xmalloc(W * H * 4);
    for (int k = 0; k < N; k++) { /* a gradient that moves */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                u8 *px = rgba + 4 * (y * W + x);
                px[0] = (u8)(4 * (x + 3 * k));
                px[1] = (u8)(5 * y);
                px[2] = (u8)(40 * k);
                px[3] = 255;
            }
        CHECK_MSG(video_file_add(vf, rgba, err, sizeof err), "%s", err);
    }
    free(rgba);
    CHECK_MSG(video_file_close(vf, path, true, err, sizeof err), "%s", err);
    check_mp4(path, W, H, N, 0.5);
    remove(path);
    /* not a size H.264 takes: odd, or too large */
    CHECK(video_file_create(path, 63, 48, 10, err, sizeof err) == NULL);
    CHECK(video_file_create(path, VIDEO_MAX_SIDE + 2, 48, 10, err, sizeof err) == NULL);
    CHECK(strstr(err, "can't be") != NULL);
    /* dropped: the file goes */
    vf = video_file_create(path, 16, 16, 30, err, sizeof err);
    CHECK(vf != NULL);
    if (vf) video_file_close(vf, path, false, err, sizeof err);
    FILE *g = fopen(path, "rb");
    CHECK(g == NULL);
    if (g) fclose(g);
}

/* RGB to Y'CbCr, BT.709 video range */
TEST(colors) {
    u8 rgba[4 * 4], Y[16 * 2], U[16], V[16];
    const u8 cols[4][3] = {{255, 255, 255}, {0, 0, 0}, {255, 0, 0}, {0, 0, 255}};
    int want[4][3] = {{235, 128, 128}, {16, 128, 128}, {63, 102, 240}, {32, 240, 118}};
    for (int c = 0; c < 4; c++) {
        for (int k = 0; k < 4; k++) { /* a 2x2 frame of one color */
            memcpy(rgba + 4 * k, cols[c], 3);
            rgba[4 * k + 3] = 255;
        }
        video_rgba_to_yuv(rgba, 2, 2, Y, 16, U, V, 16);
        CHECK_MSG(abs(Y[0] - want[c][0]) <= 1 && abs(Y[17] - want[c][0]) <= 1, "Y %d for color %d", Y[0], c);
        CHECK_MSG(abs(U[0] - want[c][1]) <= 1, "Cb %d for color %d", U[0], c);
        CHECK_MSG(abs(V[0] - want[c][2]) <= 1, "Cr %d for color %d", V[0], c);
    }
}

/* a video of a script whose @prepare moves the view with .time: the frames follow the time, and the state the job was
   given doesn't change */
TEST(script_video) {
    GT_REQUIRE();
    Program *p = LOAD_OK(
        "@config\n"
        "plane.radius = 1\n"
        "@prepare\n"
        "plane.center = .time\n"
        "@viewer plane z\n"
        "color = [fract(re(z)), 0.5, 0.5]\n");
    const char *path = "build/test_video2.mp4";
    char err[512] = "";
    u32 *state = (u32 *)xmalloc(sizeof(u32) * (size_t)p->state_words);
    memcpy(state, p->initial_state, sizeof(u32) * (size_t)p->state_words);
    VideoJob *j = video_begin(p, state, p->viewers[0], path, 32, 24, 4, 1.0, 2.0, 2, err, sizeof err);
    CHECK_MSG(j != NULL, "%s", err);
    if (!j) return;
    int r, steps = 0;
    while ((r = video_step(j)) == 1) steps++;
    CHECK_MSG(r == 0, "%s", j->err);
    CHECK_INT(steps, 3); /* (4 frames: 3 more after each of the first) */
    CHECK_NEAR(wf(j->state[p->viewers[0]->center->slot]), 2.75, 1e-6); /* the last frame: time 2 + 3/4 */
    CHECK_NEAR(wf(state[p->viewers[0]->center->slot]), 0, 0);
    video_end(j);
    check_mp4(path, 32, 24, 4, 1.0);
    remove(path);
    free(state);
    program_free(p);
    GT_NO_VALIDATION_ERRORS();
}

static const TestCase tests_video[] = {
    TESTCASE(file_boxes),
    TESTCASE(colors),
    TESTCASE(script_video),
};
