/* Host test for safe_area.h: make test */
#include "safe_area.h"

#include <stdio.h>

using safe_area::Rect;
using safe_area::Ratio;

static const Ratio k43{4, 3};   /* the core's video_freak ARX:ARY */

static int g_fail = 0;

static void expect_rect(const char *spec, Rect vis, Rect want, Ratio dar = k43) {
    Rect r{-1, -1, -1, -1};
    const char *err = nullptr;
    bool ok = safe_area::resolve(spec, 320, 240, vis, dar, &r, &err);
    if (!ok || r.x != want.x || r.y != want.y || r.w != want.w || r.h != want.h) {
        printf("FAIL %-12s vis %dx%d+%d+%d: got %s %dx%d+%d+%d, want %dx%d+%d+%d\n",
               spec, vis.w, vis.h, vis.x, vis.y, ok ? "ok" : err,
               r.w, r.h, r.x, r.y, want.w, want.h, want.x, want.y);
        g_fail++;
    }
}

static void expect_reject(const char *spec, Rect vis) {
    Rect r{7, 7, 7, 7};
    const char *err = nullptr;
    if (safe_area::resolve(spec, 320, 240, vis, k43, &r, &err) || r.x != 7) {
        printf("FAIL %-12s accepted, want rejected\n", spec);
        g_fail++;
    }
}

static void expect_cfg(const char *text, bool want_ok, const char *want) {
    char v[64] = "unset";
    bool ok = safe_area::cfg_value(text, "safe_area", v, sizeof v);
    if (ok != want_ok || (ok && strcmp(v, want))) {
        printf("FAIL cfg \"%s\": got %d \"%s\", want %d \"%s\"\n", text, ok, v, want_ok, want);
        g_fail++;
    }
}

int main() {
    const Rect full{0, 0, 320, 240};
    const Rect core224{0, 7, 320, 224};   /* the shipping core: rows 7-230 */

    expect_rect("off",        core224, full);
    expect_rect("",           core224, full);
    /* At 4:3 the 224-line window's pixels are 224/240 as wide as tall, so the
       4:3 game fills it whole; the 240-line window is 1:1. */
    expect_rect("fit",        full,    full);
    expect_rect("fit",        core224, core224);
    expect_rect(" FIT ",      core224, core224);
    expect_rect("crt",        core224, Rect{16, 18, 288, 202});
    expect_rect("90%",        core224, Rect{16, 18, 288, 202});
    expect_rect("85%",        core224, Rect{24, 24, 272, 190});
    expect_rect("100%",       core224, core224);
    expect_rect("crt",        full,    Rect{16, 12, 288, 216});
    /* Other display aspects: square pixels in the 224 window (side bars), a
       16:9 window (wider bars), a 1:1 display of the full frame (letterbox). */
    expect_rect("fit",        core224, Rect{10, 7, 299, 224}, Ratio{320, 224});
    expect_rect("fit",        core224, Rect{40, 7, 240, 224}, Ratio{16, 9});
    expect_rect("fit",        full,    Rect{0, 30, 320, 180}, Ratio{1, 1});
    expect_rect("288x216",    core224, Rect{16, 11, 288, 216});
    expect_rect("300x200+4+20", core224, Rect{4, 20, 300, 200});

    expect_reject("0%",       core224);
    expect_reject("101%",     core224);
    expect_reject("bogus",    core224);
    expect_reject("stretch",  core224);      /* removed: fit is the aspect-correct fill */
    expect_reject("320x240",  core224);      /* larger than the visible window */
    expect_reject("300x200+30+0", core224);  /* off the right edge */
    expect_reject("8x8+0+0",  core224);      /* under the minimum */
    expect_reject("300x200+4", core224);
    expect_reject("fit", Rect{0, 20, 320, 240});  /* visible window outside the frame */

    expect_cfg("safe_area=fit\n", true, "fit");
    expect_cfg("# safe_area=crt\n  safe_area = 90%   # overscan\n", true, "90%");
    expect_cfg("other=1\nsafe_area=\nsafe_area=crt\n", true, "");
    expect_cfg("safe_areax=fit\n", false, "");
    expect_cfg("", false, "");
    expect_cfg("safe_area=crt", true, "crt");

    Ratio q{0, 0};
    if (!safe_area::parse_ratio("4:3", &q) || q.num != 4 || q.den != 3) { printf("FAIL ratio 4:3\n"); g_fail++; }
    if (!safe_area::parse_ratio(" 16:9 ", &q) || q.num != 16 || q.den != 9) { printf("FAIL ratio 16:9\n"); g_fail++; }
    if (safe_area::parse_ratio("4:0", &q) || safe_area::parse_ratio("4x3", &q) || safe_area::parse_ratio("4:", &q)) {
        printf("FAIL ratio accepted a bad value\n"); g_fail++;
    }

    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("safe_area: all tests passed\n");
    return 0;
}
