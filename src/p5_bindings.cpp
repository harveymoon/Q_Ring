// =============================================================================
// Q-Ring — p5.js drawing surface, bound to LovyanGFX via mJS FFI.
//
// Architecture:
//   - C helper functions with simple int/double signatures (one per primitive)
//   - A small JS prelude (PRELUDE_JS) that wraps them as p5-style globals
//   - 8-deep transform stack (2D affine), fill/stroke/weight state, text state
//   - Alpha approximated against the last background() color (RGB565 has no
//     alpha channel — documented in SUPPORTED.md)
// =============================================================================
#include "p5_bindings.h"
#include "face.h"
#include "p5_math.h"
#include <LovyanGFX.hpp>
#include <Arduino.h>
#include <math.h>
#include <string.h>

extern "C" {
#include "vendor/mjs/mjs.h"
}

// =============================================================================
// State
// =============================================================================
struct Affine { float a, b, c, d, e, f; };  // x' = a*x + c*y + e
static inline Affine ident() { return {1,0, 0,1, 0,0}; }
static inline Affine compose(const Affine& A, const Affine& B) {
    return {
        A.a * B.a + A.c * B.b,   A.b * B.a + A.d * B.b,
        A.a * B.c + A.c * B.d,   A.b * B.c + A.d * B.d,
        A.a * B.e + A.c * B.f + A.e,
        A.b * B.e + A.d * B.f + A.f
    };
}
static inline void apply(const Affine& A, float x, float y, int& ox, int& oy) {
    ox = (int)(A.a * x + A.c * y + A.e);
    oy = (int)(A.b * x + A.d * y + A.f);
}

// "Pure translation" check — when b and c are ~0, the transform has no
// rotation or skew, so LovyanGFX's axis-aligned primitives suffice. Once
// there's any rotation we have to triangulate (LovyanGFX won't rotate
// fillEllipse / fillRect on its own).
static inline bool xform_is_axis_aligned(const Affine& A) {
    return fabsf(A.b) < 0.001f && fabsf(A.c) < 0.001f;
}

struct State {
    uint16_t fill_rgb565;
    uint16_t stroke_rgb565;
    uint16_t bg_rgb565;
    bool     no_fill;
    bool     no_stroke;
    uint8_t  weight;        // 1..8
    uint8_t  text_size;     // LovyanGFX scale
    uint8_t  text_align;    // 0=LEFT, 1=CENTER, 2=RIGHT
    Affine   xform;
    Affine   stack[8];
    uint8_t  sp;
};

static State    _st;
static LGFX_Sprite* _cv = nullptr;

static uint16_t _rgb565(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static void _from565(uint16_t c, int& r, int& g, int& b) {
    r = (c >> 11) << 3;
    g = ((c >> 5) & 0x3F) << 2;
    b = (c & 0x1F) << 3;
}
static uint16_t _alpha_blend(uint16_t fg, uint16_t bg, int a) {
    if (a >= 255) return fg;
    if (a <= 0)   return bg;
    int fr, fgc, fb, br, bg2, bb;
    _from565(fg, fr, fgc, fb);
    _from565(bg, br, bg2, bb);
    int r = (fr * a + br * (255 - a)) / 255;
    int g = (fgc * a + bg2 * (255 - a)) / 255;
    int b = (fb * a + bb * (255 - a)) / 255;
    return _rgb565(r, g, b);
}

// =============================================================================
// Drawing helpers that respect transform + state
// =============================================================================
static void _stroke_line(int x1, int y1, int x2, int y2) {
    if (_st.no_stroke) return;
    if (_st.weight <= 1) {
        _cv->drawLine(x1, y1, x2, y2, _st.stroke_rgb565);
        return;
    }
    // Thick line: perpendicular offset stamps.
    float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.5f) { _cv->fillCircle(x1, y1, _st.weight/2, _st.stroke_rgb565); return; }
    float nx = -dy / len, ny = dx / len;
    int half = _st.weight / 2;
    for (int i = -half; i <= half; i++) {
        int ox = (int)(nx * i), oy = (int)(ny * i);
        _cv->drawLine(x1 + ox, y1 + oy, x2 + ox, y2 + oy, _st.stroke_rgb565);
    }
}

// =============================================================================
// C functions — registered with mJS, called from the JS prelude.
// Naming: p5_<name> for the C side, mapped to the p5 names in PRELUDE_JS.
// =============================================================================
extern "C" {

void p5_background(int r, int g, int b) {
    _st.bg_rgb565 = _rgb565(r, g, b);
    _cv->fillScreen(_st.bg_rgb565);
}

void p5_fill(int r, int g, int b) {
    _st.fill_rgb565 = _rgb565(r, g, b);
    _st.no_fill = false;
}
void p5_fillA(int r, int g, int b, int a) {
    _st.fill_rgb565 = _alpha_blend(_rgb565(r, g, b), _st.bg_rgb565, a);
    _st.no_fill = false;
}
void p5_no_fill(void) { _st.no_fill = true; }

void p5_stroke(int r, int g, int b) {
    _st.stroke_rgb565 = _rgb565(r, g, b);
    _st.no_stroke = false;
}
void p5_strokeA(int r, int g, int b, int a) {
    _st.stroke_rgb565 = _alpha_blend(_rgb565(r, g, b), _st.bg_rgb565, a);
    _st.no_stroke = false;
}
void p5_no_stroke(void) { _st.no_stroke = true; }

void p5_stroke_weight(int w) {
    if (w < 1) w = 1; if (w > 8) w = 8;
    _st.weight = (uint8_t)w;
}

// NOTE: all drawing primitives take `int` coords because mJS's FFI dispatch
//   caps at 3 args whenever a `double` is present in the signature. 4+ doubles
//   would silently `return -1`. Pixel coords don't need sub-pixel precision.

void p5_circle(int x, int y, int d) {
    int cx, cy; apply(_st.xform, (float)x, (float)y, cx, cy);
    int r = d / 2;
    if (!_st.no_fill)   _cv->fillCircle(cx, cy, r, _st.fill_rgb565);
    if (!_st.no_stroke) {
        for (int w = 0; w < _st.weight; w++) _cv->drawCircle(cx, cy, r - w, _st.stroke_rgb565);
    }
}

// Precomputed sin/cos for the rotated-ellipse triangle fan. Saves ~25 trig
// calls per ellipse on every frame. N=12 segments looks smooth at 240x240.
#define ELL_N 12
static float _ell_cos[ELL_N + 1];
static float _ell_sin[ELL_N + 1];
static bool  _ell_table_ready = false;
static void _ensure_ellipse_table() {
    if (_ell_table_ready) return;
    for (int i = 0; i <= ELL_N; i++) {
        float t = (float)i / (float)ELL_N * 2.0f * (float)M_PI;
        _ell_cos[i] = cosf(t);
        _ell_sin[i] = sinf(t);
    }
    _ell_table_ready = true;
}

void p5_ellipse(int x, int y, int w, int h) {
    if (_st.no_fill && _st.no_stroke) return;
    int rw = w / 2, rh = h / 2;
    if (rw <= 0 || rh <= 0) return;

    if (xform_is_axis_aligned(_st.xform)) {
        int cx, cy; apply(_st.xform, (float)x, (float)y, cx, cy);
        if (!_st.no_fill)   _cv->fillEllipse(cx, cy, rw, rh, _st.fill_rgb565);
        if (!_st.no_stroke)
            for (int k = 0; k < _st.weight; k++)
                _cv->drawEllipse(cx, cy, rw - k, rh - k, _st.stroke_rgb565);
        return;
    }

    _ensure_ellipse_table();
    int cx0, cy0; apply(_st.xform, (float)x, (float)y, cx0, cy0);
    int prev_x = 0, prev_y = 0;
    for (int i = 0; i <= ELL_N; i++) {
        float lx = (float)x + (float)rw * _ell_cos[i];
        float ly = (float)y + (float)rh * _ell_sin[i];
        int px, py; apply(_st.xform, lx, ly, px, py);
        if (i > 0) {
            if (!_st.no_fill)   _cv->fillTriangle(cx0, cy0, prev_x, prev_y, px, py, _st.fill_rgb565);
            if (!_st.no_stroke) _stroke_line(prev_x, prev_y, px, py);
        }
        prev_x = px; prev_y = py;
    }
}

void p5_rect(int x, int y, int w, int h) {
    if (_st.no_fill && _st.no_stroke) return;

    if (xform_is_axis_aligned(_st.xform)) {
        int x0, y0; apply(_st.xform, (float)x, (float)y, x0, y0);
        if (!_st.no_fill)   _cv->fillRect(x0, y0, w, h, _st.fill_rgb565);
        if (!_st.no_stroke)
            for (int k = 0; k < _st.weight; k++)
                _cv->drawRect(x0 + k, y0 + k, w - 2*k, h - 2*k, _st.stroke_rgb565);
        return;
    }

    // Rotated rect: two triangles, four stroked edges.
    int ax, ay; apply(_st.xform, (float)x,       (float)y,       ax, ay);
    int bx, by; apply(_st.xform, (float)(x + w), (float)y,       bx, by);
    int cx, cy; apply(_st.xform, (float)(x + w), (float)(y + h), cx, cy);
    int dx, dy; apply(_st.xform, (float)x,       (float)(y + h), dx, dy);
    if (!_st.no_fill) {
        _cv->fillTriangle(ax, ay, bx, by, cx, cy, _st.fill_rgb565);
        _cv->fillTriangle(ax, ay, cx, cy, dx, dy, _st.fill_rgb565);
    }
    if (!_st.no_stroke) {
        _stroke_line(ax, ay, bx, by);
        _stroke_line(bx, by, cx, cy);
        _stroke_line(cx, cy, dx, dy);
        _stroke_line(dx, dy, ax, ay);
    }
}

void p5_rect_r(int x, int y, int w, int h, int r) {
    // Rounded rect only honors corner radius when axis-aligned. Falls back to
    // a sharp-cornered rotated rect when rotated (LovyanGFX has no rotated
    // round-rect primitive and approximating the corners properly is overkill
    // for the gift's use cases).
    if (xform_is_axis_aligned(_st.xform)) {
        int x0, y0; apply(_st.xform, (float)x, (float)y, x0, y0);
        if (!_st.no_fill)   _cv->fillRoundRect(x0, y0, w, h, r, _st.fill_rgb565);
        if (!_st.no_stroke)
            for (int k = 0; k < _st.weight; k++)
                _cv->drawRoundRect(x0 + k, y0 + k, w - 2*k, h - 2*k, r, _st.stroke_rgb565);
        return;
    }
    p5_rect(x, y, w, h);
}

void p5_line(int x1, int y1, int x2, int y2) {
    int a, b, c, d;
    apply(_st.xform, (float)x1, (float)y1, a, b);
    apply(_st.xform, (float)x2, (float)y2, c, d);
    _stroke_line(a, b, c, d);
}

void p5_point(int x, int y) {
    int px, py; apply(_st.xform, (float)x, (float)y, px, py);
    if (_st.no_stroke) return;
    if (_st.weight <= 1) _cv->drawPixel(px, py, _st.stroke_rgb565);
    else                  _cv->fillCircle(px, py, _st.weight / 2, _st.stroke_rgb565);
}

void p5_triangle(int x1, int y1, int x2, int y2, int x3, int y3) {
    int a, b, c, d, e, f;
    apply(_st.xform, (float)x1, (float)y1, a, b);
    apply(_st.xform, (float)x2, (float)y2, c, d);
    apply(_st.xform, (float)x3, (float)y3, e, f);
    if (!_st.no_fill)   _cv->fillTriangle(a, b, c, d, e, f, _st.fill_rgb565);
    if (!_st.no_stroke) {
        _stroke_line(a, b, c, d);
        _stroke_line(c, d, e, f);
        _stroke_line(e, f, a, b);
    }
}

// arc angles arrive as degrees * 10 (so a full circle = 3600) — the JS wrapper
// converts radians to this fixed-point representation before the FFI call.
void p5_arc(int x, int y, int w, int h, int start_deg10, int stop_deg10) {
    int cx, cy; apply(_st.xform, (float)x, (float)y, cx, cy);
    int rw = w / 2, rh = h / 2;
    float a0 = (float)start_deg10 / 10.0f;
    float a1 = (float)stop_deg10  / 10.0f;
    if (!_st.no_fill)   _cv->fillArc(cx, cy, rw, rh > 0 ? rh : rw, a0, a1, _st.fill_rgb565);
    if (!_st.no_stroke) _cv->drawArc(cx, cy, rw, rh > 0 ? rh : rw, a0, a1, _st.stroke_rgb565);
}

void p5_text(const char* s, int x, int y) {
    int px, py; apply(_st.xform, (float)x, (float)y, px, py);
    _cv->setTextSize(_st.text_size);
    _cv->setTextColor(_st.fill_rgb565);
    int th = 8 * _st.text_size;

    // Walk lines split on '\n'. Each line is independently aligned and
    // positioned so multi-line strings track textAlign + the anchor (px,py)
    // instead of snapping back to canvas X=0 the way LovyanGFX's built-in
    // wrap does. First line baseline matches the original single-line call.
    const char* line = s;
    int line_y = py - th;
    char buf[128];
    while (true) {
        const char* nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, line, len);
        buf[len] = '\0';

        int tw = (int)_cv->textWidth(buf);
        int ox = px;
        if (_st.text_align == 1)      ox = px - tw / 2;
        else if (_st.text_align == 2) ox = px - tw;
        _cv->setCursor(ox, line_y);
        _cv->print(buf);

        if (!nl) break;
        line = nl + 1;
        line_y += th;
    }
}

void p5_text_size(int n) {
    if (n < 8) n = 8; if (n > 48) n = 48;
    _st.text_size = (uint8_t)(n / 8);
    if (_st.text_size < 1) _st.text_size = 1;
}
void p5_text_align(int a) {
    _st.text_align = (uint8_t)((a < 0 || a > 2) ? 0 : a);
}

void p5_translate(int x, int y) {
    Affine t = {1, 0, 0, 1, (float)x, (float)y};
    _st.xform = compose(_st.xform, t);
}
void p5_rotate(double rad) {
    float c = cosf((float)rad), s = sinf((float)rad);
    Affine t = {c, s, -s, c, 0, 0};
    _st.xform = compose(_st.xform, t);
}
void p5_scale(double s) {
    Affine t = {(float)s, 0, 0, (float)s, 0, 0};
    _st.xform = compose(_st.xform, t);
}
void p5_push(void) {
    if (_st.sp < 8) _st.stack[_st.sp++] = _st.xform;
}
void p5_pop(void) {
    if (_st.sp > 0) _st.xform = _st.stack[--_st.sp];
}

// Math (mJS has Math.sin etc., but expose these for parity with p5 globals)
double p5_random(double lo, double hi)   { return (double)p5m_random_range((float)lo, (float)hi); }
double p5_noise(double x, double y, double z) { return (double)p5m_noise3((float)x, (float)y, (float)z); }
double p5_map(double v, double a, double b, double c, double d) { return (double)p5m_map((float)v, (float)a, (float)b, (float)c, (float)d); }
double p5_lerp(double a, double b, double t)  { return (double)p5m_lerp((float)a, (float)b, (float)t); }
double p5_dist(double x1, double y1, double x2, double y2) { return (double)p5m_dist((float)x1, (float)y1, (float)x2, (float)y2); }
double p5_constrain(double v, double lo, double hi) { return (double)p5m_constrain((float)v, (float)lo, (float)hi); }
double p5_radians(double d) { return d * M_PI / 180.0; }
double p5_degrees(double r) { return r * 180.0 / M_PI; }

int    p5_millis(void) { return (int)millis(); }

void p5_no_auto_rotate(void) {
    face_set_rotation_auto(false);
    face_set_rotation(0);
}
void p5_auto_rotate(void) { face_set_rotation_auto(true); }

// Number-to-string helper. mJS forbids `"text " + number` implicit conversion;
// sketches use `str(n)` to format numbers for concatenation. Static buffer
// rotates between calls so a single expression with up to 4 nested str() calls
// works without the strings clobbering one another.
const char* p5_str(double n) {
    static char bufs[4][32];
    static int  idx = 0;
    char* buf = bufs[idx]; idx = (idx + 1) & 3;
    double a = n < 0 ? -n : n;
    if (a == (long)a && a < 1e9) snprintf(buf, 32, "%ld", (long)n);
    else                         snprintf(buf, 32, "%g", n);
    return buf;
}

} // extern "C"

// =============================================================================
// JS prelude — wraps C helpers as p5-style global functions.
// =============================================================================
static const char* PRELUDE_JS =
// FFI binding declarations — int args (mJS limits to 3 args when doubles
//   are involved, so all drawing prims now take int coords).
"let _bg=ffi('void p5_background(int,int,int)');"
"let _fl=ffi('void p5_fill(int,int,int)');"
"let _fla=ffi('void p5_fillA(int,int,int,int)');"
"let _nf=ffi('void p5_no_fill()');"
"let _sk=ffi('void p5_stroke(int,int,int)');"
"let _ska=ffi('void p5_strokeA(int,int,int,int)');"
"let _ns=ffi('void p5_no_stroke()');"
"let _sw=ffi('void p5_stroke_weight(int)');"
"let _ci=ffi('void p5_circle(int,int,int)');"
"let _el=ffi('void p5_ellipse(int,int,int,int)');"
"let _re=ffi('void p5_rect(int,int,int,int)');"
"let _rer=ffi('void p5_rect_r(int,int,int,int,int)');"
"let _ln=ffi('void p5_line(int,int,int,int)');"
"let _pt=ffi('void p5_point(int,int)');"
"let _tr=ffi('void p5_triangle(int,int,int,int,int,int)');"
"let _ar=ffi('void p5_arc(int,int,int,int,int,int)');"  // angles in degrees*10
"let _tx=ffi('void p5_text(char*,int,int)');"
"let _ts=ffi('void p5_text_size(int)');"
"let _ta=ffi('void p5_text_align(int)');"
"let _t=ffi('void p5_translate(int,int)');"
"let _ro=ffi('void p5_rotate(double)');"
"let _sc=ffi('void p5_scale(double)');"
"let _ph=ffi('void p5_push()');"
"let _po=ffi('void p5_pop()');"
"let _rn=ffi('double p5_random(double,double)');"
"let _no=ffi('double p5_noise(double,double,double)');"
"let _ms=ffi('int p5_millis()');"
"let str=ffi('char* p5_str(double)');"
"let noAutoRotate=ffi('void p5_no_auto_rotate()');"
"let autoRotate=ffi('void p5_auto_rotate()');"

// p5-named globals. mJS is strict about braced if/else, so every conditional
// uses explicit blocks.
"function background(r,g,b){ if(g===undefined){ g=r; b=r; } _bg(r,g,b); }\n"
"function fill(r,g,b,a){ if(g===undefined){ g=r; b=r; } if(a===undefined){ _fl(r,g,b); } else { _fla(r,g,b,a); } }\n"
"function noFill(){ _nf(); }\n"
"function stroke(r,g,b,a){ if(g===undefined){ g=r; b=r; } if(a===undefined){ _sk(r,g,b); } else { _ska(r,g,b,a); } }\n"
"function noStroke(){ _ns(); }\n"
"function strokeWeight(w){ _sw(w); }\n"
"function circle(x,y,d){ _ci(x,y,d); }\n"
"function ellipse(x,y,w,h){ if(h===undefined){ h=w; } _el(x,y,w,h); }\n"
"function rect(x,y,w,h,r){ if(r===undefined){ _re(x,y,w,h); } else { _rer(x,y,w,h,r); } }\n"
"function line(x1,y1,x2,y2){ _ln(x1,y1,x2,y2); }\n"
"function point(x,y){ _pt(x,y); }\n"
"function triangle(a,b,c,d,e,f){ _tr(a,b,c,d,e,f); }\n"
// arc takes radians in p5. Convert to degrees*10 to fit the int-only FFI.
"function arc(x,y,w,h,s,t){ _ar(x,y,w,h, floor(s*1800/PI), floor(t*1800/PI)); }\n"
"function text(s,x,y){ _tx(''+s,x,y); }\n"
"function textSize(n){ _ts(n); }\n"
"function textAlign(a){ _ta(a); }\n"
"function translate(x,y){ _t(x,y); }\n"
"function rotate(r){ _ro(r); }\n"
"function scale(s){ _sc(s); }\n"
"function push(){ _ph(); }\n"
"function pop(){ _po(); }\n"
"function random(a,b){ if(a===undefined){ return _rn(0,1); } if(b===undefined){ return _rn(0,a); } return _rn(a,b); }\n"
"function noise(x,y,z){ if(y===undefined){ y=0; } if(z===undefined){ z=0; } return _no(x,y,z); }\n"
// Multi-arg math helpers — pure JS because mJS's FFI can't pass 4+ doubles.
"function map(v,a,b,c,d){ if(b===a){ return c; } return c + (v - a) * (d - c) / (b - a); }\n"
"function lerp(a,b,t){ return a + (b - a) * t; }\n"
"function dist(x1,y1,x2,y2){ let dx=x2-x1; let dy=y2-y1; return sqrt(dx*dx + dy*dy); }\n"
"function constrain(v,lo,hi){ if(v<lo){ return lo; } if(v>hi){ return hi; } return v; }\n"
"function radians(d){ return d * PI / 180; }\n"
"function degrees(r){ return r * 180 / PI; }\n"
"function millis(){ return _ms(); }\n"
"function centerOrigin(){ translate(width/2, height/2); }\n"
// p5 math globals — bound directly to libc's sin/cos/... via the FFI resolver.
"let sin = ffi('double sin(double)');\n"
"let cos = ffi('double cos(double)');\n"
"let tan = ffi('double tan(double)');\n"
"let asin = ffi('double asin(double)');\n"
"let acos = ffi('double acos(double)');\n"
"let atan = ffi('double atan(double)');\n"
"let atan2 = ffi('double atan2(double,double)');\n"
"let sqrt = ffi('double sqrt(double)');\n"
"let pow = ffi('double pow(double,double)');\n"
"let exp = ffi('double exp(double)');\n"
"let log = ffi('double log(double)');\n"
"let abs = ffi('double fabs(double)');\n"
"let floor = ffi('double floor(double)');\n"
"let ceil = ffi('double ceil(double)');\n"
"let round = ffi('double round(double)');\n"
"function min(a,b){ if(a<b){ return a; } return b; }\n"
"function max(a,b){ if(a>b){ return a; } return b; }\n"
// Constants
"let LEFT=0, CENTER=1, RIGHT=2;\n"
"let PI=3.141592653589793, TWO_PI=6.283185307179586, HALF_PI=1.5707963267948966;\n"
"let width=240, height=240;\n"
"let frameCount=0, accelX=0, accelY=0, accelZ=0;\n"
"function setup(){} function draw(){}\n"
;

// Resolver — mJS's `ffi('...')` calls this to look up native function pointers
// by name. We can't use dlsym on RP2040, so we hand-roll a small table.
struct FnEntry { const char* name; void* fn; };
static const FnEntry FN_TABLE[] = {
    { "p5_background",    (void*)p5_background },
    { "p5_fill",          (void*)p5_fill },
    { "p5_fillA",         (void*)p5_fillA },
    { "p5_no_fill",       (void*)p5_no_fill },
    { "p5_stroke",        (void*)p5_stroke },
    { "p5_strokeA",       (void*)p5_strokeA },
    { "p5_no_stroke",     (void*)p5_no_stroke },
    { "p5_stroke_weight", (void*)p5_stroke_weight },
    { "p5_circle",        (void*)p5_circle },
    { "p5_ellipse",       (void*)p5_ellipse },
    { "p5_rect",          (void*)p5_rect },
    { "p5_rect_r",        (void*)p5_rect_r },
    { "p5_line",          (void*)p5_line },
    { "p5_point",         (void*)p5_point },
    { "p5_triangle",      (void*)p5_triangle },
    { "p5_arc",           (void*)p5_arc },
    { "p5_text",          (void*)p5_text },
    { "p5_text_size",     (void*)p5_text_size },
    { "p5_text_align",    (void*)p5_text_align },
    { "p5_translate",     (void*)p5_translate },
    { "p5_rotate",        (void*)p5_rotate },
    { "p5_scale",         (void*)p5_scale },
    { "p5_push",          (void*)p5_push },
    { "p5_pop",           (void*)p5_pop },
    { "p5_random",        (void*)p5_random },
    { "p5_noise",         (void*)p5_noise },
    { "p5_map",           (void*)p5_map },
    { "p5_lerp",           (void*)p5_lerp },
    { "p5_dist",          (void*)p5_dist },
    { "p5_constrain",     (void*)p5_constrain },
    { "p5_radians",       (void*)p5_radians },
    { "p5_degrees",       (void*)p5_degrees },
    { "p5_millis",        (void*)p5_millis },
    { "p5_str",           (void*)p5_str },
    { "p5_no_auto_rotate",(void*)p5_no_auto_rotate },
    { "p5_auto_rotate",   (void*)p5_auto_rotate },
    // libc math, bound directly — p5 exposes these as bare globals.
    { "sin",   (void*)(double(*)(double))sin },
    { "cos",   (void*)(double(*)(double))cos },
    { "tan",   (void*)(double(*)(double))tan },
    { "asin",  (void*)(double(*)(double))asin },
    { "acos",  (void*)(double(*)(double))acos },
    { "atan",  (void*)(double(*)(double))atan },
    { "atan2", (void*)(double(*)(double,double))atan2 },
    { "sqrt",  (void*)(double(*)(double))sqrt },
    { "pow",   (void*)(double(*)(double,double))pow },
    { "exp",   (void*)(double(*)(double))exp },
    { "log",   (void*)(double(*)(double))log },
    { "fabs",  (void*)(double(*)(double))fabs },
    { "floor", (void*)(double(*)(double))floor },
    { "ceil",  (void*)(double(*)(double))ceil },
    { "round", (void*)(double(*)(double))round },
};

extern "C" void* qring_ffi_resolver(void* handle, const char* name) {
    (void)handle;
    for (size_t i = 0; i < sizeof(FN_TABLE) / sizeof(FN_TABLE[0]); i++) {
        if (strcmp(FN_TABLE[i].name, name) == 0) return FN_TABLE[i].fn;
    }
    return nullptr;
}

void p5_bindings_install(struct mjs* m) {
    _cv = face_get_canvas();
    memset(&_st, 0, sizeof(_st));
    _st.xform = ident();
    _st.weight = 1;
    _st.text_size = 2;
    _st.fill_rgb565 = 0xFFFF;
    _st.stroke_rgb565 = 0xFFFF;
    _st.bg_rgb565 = 0x0000;
    _st.no_stroke = true;   // p5 default: filled, no stroke
    // Long strings should clip at the canvas edge — sketches that want
    // multi-line layout do it explicitly with multiple text() calls.
    _cv->setTextWrap(false, false);

    // Register the resolver — mJS will call this for every ffi('...') in the
    // prelude. No dlsym needed; our small static table handles every p5 name.
    mjs_set_ffi_resolver(m, (mjs_ffi_resolver_t*)qring_ffi_resolver);
}

const char* p5_bindings_prelude_js(void) { return PRELUDE_JS; }

void p5_bindings_reset_state(void) {
    _st.xform = ident();
    _st.sp = 0;
    _st.weight = 1;
    _st.no_fill = false;
    _st.no_stroke = true;
    _st.text_align = 0;
    _st.text_size = 2;
}

void p5_bindings_update_globals(struct mjs* m, uint32_t frame_count) {
    mjs_set(m, mjs_get_global(m), "frameCount", ~0, mjs_mk_number(m, (double)frame_count));
    extern float imu_get_ax(); extern float imu_get_ay(); extern float imu_get_az();
    // The QMI8658 is mounted with its +X/+Y axes pointing left/up relative
    // to the screen. Negate at this layer so sketches see screen-aligned
    // values: accelX +ve = tilted right, accelY +ve = tilted toward bottom.
    // (The IMU driver and face auto-rotate still use the raw values.)
    mjs_set(m, mjs_get_global(m), "accelX", ~0, mjs_mk_number(m, -(double)imu_get_ax()));
    mjs_set(m, mjs_get_global(m), "accelY", ~0, mjs_mk_number(m, -(double)imu_get_ay()));
    mjs_set(m, mjs_get_global(m), "accelZ", ~0, mjs_mk_number(m, (double)imu_get_az()));
    // User-picked accent color is also exposed so sketches can theme to match.
    mjs_set(m, mjs_get_global(m), "themeR", ~0, mjs_mk_number(m, (double)face_get_accent_r()));
    mjs_set(m, mjs_get_global(m), "themeG", ~0, mjs_mk_number(m, (double)face_get_accent_g()));
    mjs_set(m, mjs_get_global(m), "themeB", ~0, mjs_mk_number(m, (double)face_get_accent_b()));
}
