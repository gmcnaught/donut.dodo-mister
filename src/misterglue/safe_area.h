/*
  Safe area: where on the scanout the finished picture lands.

  The core does not show every line of the 320x240 frame (the shipping core
  shows rows 7-230, the 224 lines a CRT displays), and a CRT or an overscanning
  TV hides more. The safe area shrinks the final picture into a rect the viewer
  actually sees. Only draws into the default framebuffer are remapped; the game
  keeps rendering its 320x240 surface at native size, so this is one scaled
  composite, not a change to every sprite.

  Spec strings (MISTER_GLUE_SAFE_AREA, or safe_area= in the display.cfg file):
    off            the whole frame, 1:1 (no scaling)
    fit            the largest rect inside the visible window that DISPLAYS at
                   the frame's aspect (see below)
    NN%            fit, scaled to NN% (1-100) on both axes and centred: margin
                   for CRT overscan
    crt            90%
    WxH            a WxH rect centred in the visible window
    WxH+X+Y        a WxH rect at X,Y in frame coordinates

  "Visible window" is the part of the frame the core scans out
  (MISTER_GLUE_VISIBLE, WxH+X+Y, default: the whole frame).

  Aspect. The core shows whatever lines it scans out at a fixed display aspect
  (MISTER_GLUE_DAR, the RBF's video_freak ARX:ARY, default the frame's own
  W:H), so a scanned pixel is square only when the window has that aspect.
  fit works in display units: the 320x224 window at 4:3 has pixels 224/240 as
  wide as tall, so the 4:3 game fills it whole (320x224, every row resampled
  240 -> 224, no side bars); the 320x240 window at 4:3 is 1:1.

  Pure: no I/O, no globals, so the parsing is unit-tested on the host.
*/
#ifndef MISTER_SAFE_AREA_H
#define MISTER_SAFE_AREA_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace safe_area {

struct Rect { int x, y, w, h; };
struct Ratio { int num, den; };   /* display width : height */

enum { kMinSize = 16 };

inline bool rect_in(const Rect &r, int fb_w, int fb_h) {
    return r.w >= kMinSize && r.h >= kMinSize && r.x >= 0 && r.y >= 0 &&
           r.x + r.w <= fb_w && r.y + r.h <= fb_h;
}

inline const char *skip_ws(const char *s) { while (*s && isspace((unsigned char)*s)) s++; return s; }

/* Strict non-negative decimal; advances *s. */
inline bool parse_uint(const char **s, int *out) {
    const char *p = *s;
    if (!isdigit((unsigned char)*p)) return false;
    long v = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); if (v > 100000) return false; p++; }
    *s = p; *out = (int)v;
    return true;
}

/* "WxH" or "WxH+X+Y" (surrounding whitespace allowed). has_pos says which. */
inline bool parse_geometry(const char *s, Rect *r, bool *has_pos) {
    s = skip_ws(s);
    Rect t{0, 0, 0, 0};
    if (!parse_uint(&s, &t.w) || (*s != 'x' && *s != 'X')) return false;
    s++;
    if (!parse_uint(&s, &t.h)) return false;
    *has_pos = false;
    if (*s == '+') {
        s++;
        if (!parse_uint(&s, &t.x) || *s != '+') return false;
        s++;
        if (!parse_uint(&s, &t.y)) return false;
        *has_pos = true;
    }
    if (*skip_ws(s)) return false;
    *r = t;
    return true;
}

/* A w x h rect centred in vis (integer, rounding the margin down). */
inline Rect centre_in(const Rect &vis, int w, int h) {
    return Rect{vis.x + (vis.w - w) / 2, vis.y + (vis.h - h) / 2, w, h};
}

/* "W:H" (surrounding whitespace allowed), both > 0. */
inline bool parse_ratio(const char *s, Ratio *r) {
    s = skip_ws(s);
    Ratio t{0, 0};
    if (!parse_uint(&s, &t.num) || *s != ':') return false;
    s++;
    if (!parse_uint(&s, &t.den)) return false;
    if (*skip_ws(s) || t.num <= 0 || t.den <= 0) return false;
    *r = t;
    return true;
}

/* The largest rect inside vis that displays at the frame's aspect (fb_w:fb_h)
   when vis is shown at dar, scaled by pct/100 on both axes, centred in vis.
   A scanned pixel's aspect (width / height) is par = dar * vis.h / vis.w, so a
   w x h rect displays at (w / h) * par; the frame wants w / h = (fb_w / fb_h) / par. */
inline Rect fit_in(const Rect &vis, int fb_w, int fb_h, Ratio dar, int pct) {
    const double par  = ((double)dar.num * vis.h) / ((double)dar.den * vis.w);
    const double want = ((double)fb_w / fb_h) / par;   /* target w / h in scanned pixels */
    double w = vis.w, h = vis.h;
    if (w / h > want) w = h * want;   /* window wider than the picture: side bars */
    else              h = w / want;   /* narrower: bars top and bottom */
    const double s = pct / 100.0;
    int iw = (int)(w * s + 0.5), ih = (int)(h * s + 0.5);
    if (iw > vis.w) iw = vis.w;
    if (ih > vis.h) ih = vis.h;
    return centre_in(vis, iw, ih);
}

/* Resolve a spec against the frame and the visible window (shown at dar). On
   failure returns false, leaves *out untouched and points *err at a reason. */
inline bool resolve(const char *spec, int fb_w, int fb_h, const Rect &vis, Ratio dar,
                    Rect *out, const char **err) {
    const char *dummy;
    if (!err) err = &dummy;
    if (!rect_in(vis, fb_w, fb_h)) { *err = "visible window outside the frame"; return false; }
    if (dar.num <= 0 || dar.den <= 0) { *err = "display aspect must be positive"; return false; }
    const char *s = skip_ws(spec ? spec : "");
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    char buf[64];
    if (n >= sizeof buf) { *err = "too long"; return false; }
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[n] = 0;

    Rect r;
    if (!n || !strcmp(buf, "off")) {
        r = Rect{0, 0, fb_w, fb_h};
    } else if (!strcmp(buf, "fit")) {
        r = fit_in(vis, fb_w, fb_h, dar, 100);
    } else if (!strcmp(buf, "crt")) {
        r = fit_in(vis, fb_w, fb_h, dar, 90);
    } else if (n > 1 && buf[n - 1] == '%') {
        const char *p = buf;
        int pct;
        if (!parse_uint(&p, &pct) || *p != '%' || pct < 1 || pct > 100) { *err = "percentage must be 1-100%"; return false; }
        r = fit_in(vis, fb_w, fb_h, dar, pct);
    } else {
        bool has_pos;
        if (!parse_geometry(buf, &r, &has_pos)) { *err = "expected off, fit, crt, NN%, WxH or WxH+X+Y"; return false; }
        if (!has_pos) {
            if (r.w > vis.w || r.h > vis.h) { *err = "WxH larger than the visible window"; return false; }
            r = centre_in(vis, r.w, r.h);
        }
    }
    if (!rect_in(r, fb_w, fb_h)) { *err = "rect outside the frame (or under 16 px)"; return false; }
    *out = r;
    return true;
}

/* The value of the first "safe_area = <value>" line of a display.cfg text into
   val (blank lines and # comments ignored). Returns false if there is none. */
inline bool cfg_value(const char *text, const char *key, char *val, size_t val_sz) {
    const size_t klen = strlen(key);
    for (const char *line = text; line && *line; ) {
        const char *eol = strchr(line, '\n');
        const char *end = eol ? eol : line + strlen(line);
        const char *p = skip_ws(line);
        if (p < end && *p != '#' && (size_t)(end - p) > klen && !strncmp(p, key, klen)) {
            const char *q = skip_ws(p + klen);
            if (q < end && *q == '=') {
                q = skip_ws(q + 1);
                if (q > end) q = end;   /* empty value: skip_ws ran past the newline */
                const char *e = end;
                const char *hash = (const char *)memchr(q, '#', (size_t)(e - q));
                if (hash) e = hash;
                while (e > q && isspace((unsigned char)e[-1])) e--;
                size_t n = (size_t)(e - q);
                if (n >= val_sz) n = val_sz - 1;
                memcpy(val, q, n);
                val[n] = 0;
                return true;
            }
        }
        line = eol ? eol + 1 : nullptr;
    }
    return false;
}

} /* namespace safe_area */

#endif
