/* test_main.c - test runner.  ./build.sh test [substring-filter]
 *
 * A test file defines tests as `TEST(name) { ... CHECK(cond); ... }` and ends with
 *   static const TestCase tests_<module>[] = { TESTCASE(name), ... };
 * and is included below.  CHECK failures are reported and the test continues;
 * SKIP("why") ends the test as skipped (e.g. no GPU available). */
#include "../src/all.c"

typedef struct TestCase { const char *name; void (*fn)(void); } TestCase;
#define TEST(name) static void test_##name(void)
#define TESTCASE(name) { #name, test_##name }

static int test_failures_ = 0;
static int test_skipped_ = 0;
static const char *test_skip_reason_ = NULL;

#define CHECK(c) do { if (!(c)) { test_failures_++; \
    printf("    CHECK failed: %s  (%s:%d)\n", #c, __FILE__, __LINE__); } } while (0)
#define CHECK_MSG(c, ...) do { if (!(c)) { test_failures_++; \
    printf("    CHECK failed: %s  (%s:%d): ", #c, __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_INT(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { test_failures_++; \
    printf("    CHECK failed: %s == %s  (%lld vs %lld)  (%s:%d)\n", #a, #b, a_, b_, __FILE__, __LINE__); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double a_ = (double)(a), b_ = (double)(b); if (!(fabs(a_ - b_) <= (eps))) { test_failures_++; \
    printf("    CHECK failed: %s ~= %s  (%.9g vs %.9g)  (%s:%d)\n", #a, #b, a_, b_, __FILE__, __LINE__); } } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); if (!a_ || !b_ || strcmp(a_, b_) != 0) { test_failures_++; \
    printf("    CHECK failed: %s == %s\n      got:      \"%s\"\n      expected: \"%s\"  (%s:%d)\n", #a, #b, a_ ? a_ : "(null)", b_ ? b_ : "(null)", __FILE__, __LINE__); } } while (0)
#define SKIP(why) do { test_skip_reason_ = (why); return; } while (0)

#if __has_include("test_lang.c")
#include "test_lang.c"
#endif
#if __has_include("test_gfx.c")
#include "test_gfx.c"
#endif
#if __has_include("test_ui.c")
#include "test_ui.c"
#endif
#if __has_include("test_editor.c")
#include "test_editor.c"
#endif
#if __has_include("test_app.c")
#include "test_app.c"
#endif

typedef struct TestSuite { const char *name; const TestCase *cases; int count; } TestSuite;
#define SUITE(arr) { #arr, arr, (int)(sizeof(arr) / sizeof(arr[0])) }

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    TestSuite suites[] = {
#if __has_include("test_lang.c")
        SUITE(tests_lang),
#endif
#if __has_include("test_gfx.c")
        SUITE(tests_gfx),
#endif
#if __has_include("test_ui.c")
        SUITE(tests_ui),
#endif
#if __has_include("test_editor.c")
        SUITE(tests_editor),
#endif
#if __has_include("test_app.c")
        SUITE(tests_app),
#endif
        { "none", NULL, 0 }
    };
    int run = 0, failed = 0;
    double t0 = fs_time_seconds();
    for (int s = 0; s < (int)(sizeof(suites) / sizeof(suites[0])); s++) {
        for (int i = 0; i < suites[s].count; i++) {
            const TestCase *tc = &suites[s].cases[i];
            char full[256];
            snprintf(full, sizeof full, "%s.%s", suites[s].name + 6, tc->name);
            if (filter && !strstr(full, filter)) continue;
            int before = test_failures_;
            test_skip_reason_ = NULL;
            double t = fs_time_seconds();
            tc->fn();
            double dt = fs_time_seconds() - t;
            run++;
            if (test_failures_ != before) {
                failed++;
                printf("FAIL %s\n", full);
            } else if (test_skip_reason_) {
                test_skipped_++;
                printf("skip %s (%s)\n", full, test_skip_reason_);
            } else {
                printf("ok   %s%s\n", full, dt > 0.5 ? "  (slow)" : "");
            }
            fflush(stdout);
        }
    }
    printf("\n%d tests, %d failed, %d skipped (%.2fs)\n", run, failed, test_skipped_, fs_time_seconds() - t0);
    return failed ? 1 : 0;
}
