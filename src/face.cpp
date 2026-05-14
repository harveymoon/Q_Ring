#include "face.h"
#include "imu.h"
#include "config.h"
#include <Arduino.h>
#include <math.h>

// =============================================================================
// Colors. C_BLACK/WHITE/RED/DRED are constants (background + angry-state iris).
// C_CYAN/DCYAN/DKCYAN are runtime variables so face_set_accent can retheme
// the face from a user-picked color (saved to /sketches/.settings).
// =============================================================================
#define C_BLACK     0x0000u
#define C_WHITE     0xFFFFu
#define C_RED       0xF800u
#define C_DRED      0x7800u

static uint8_t  _accent_r = 0;
static uint8_t  _accent_g = 200;
static uint8_t  _accent_b = 255;
static uint16_t C_CYAN    = 0x07FFu;   // main accent — iris/brow/mouth
static uint16_t C_DCYAN   = 0x0318u;   // 40% of accent — eye outer
static uint16_t C_DKCYAN  = 0x0010u;   // 10% of accent — alert background

static inline uint16_t _rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// =============================================================================
// Layout constants — 240x240 canvas
// =============================================================================
#define SCREEN_CX   120
#define SCREEN_CY   120

#define L_EYE_CX     78
#define R_EYE_CX    162
#define EYE_CY      108

#define EYE_OUTER_R  36
#define EYE_IRIS_R   25
#define EYE_PUPIL_R  13
#define EYE_HLIGHT_R  4

#define BROW_Y_BASE  (EYE_CY - EYE_OUTER_R - 9)
#define BROW_HALF_W  25
#define BROW_THICK    5

#define MOUTH_Y     165
#define MOUTH_HW     34
#define MOUTH_THICK   4

#define ALERT_Y     173
#define ALERT_H      38
#define ALERT_X      23
#define ALERT_W     194
#define ALERT_R       4

struct EyeParams {
    float outer_r, iris_r, pupil_r;
    float lid_top, lid_bot;
    float look_dx, look_dy;
    float lid_slant;
    uint16_t iris_color;
};
struct BrowParams { float inner_y, outer_y, raise; };
struct MouthParams { float smile, open_h; };

// Non-const so face_set_accent can refresh iris_color when the user picks
// a new accent.
static EyeParams         PARAMS_DEFAULT = { EYE_OUTER_R, EYE_IRIS_R, EYE_PUPIL_R, 0,0,0,0,0, C_CYAN };
static const BrowParams  BROW_DEFAULT   = { 0, 0, 0 };
static const MouthParams MOUTH_DEFAULT  = { 0, 0 };

// =============================================================================
// Live state
// =============================================================================
static LGFX_Sprite         _canvas;
static lgfx::LGFX_Device*  _lcd          = nullptr;
static FaceState            _state        = FaceState::IDLE;
static uint32_t             _state_ms     = 0;
static uint32_t             _last_ms      = 0;
static float                _smooth_angle = 0.0f;
static bool                 _rotation_auto = true;

static EyeParams   _left_cur  = PARAMS_DEFAULT, _left_tgt  = PARAMS_DEFAULT;
static EyeParams   _right_cur = PARAMS_DEFAULT, _right_tgt = PARAMS_DEFAULT;
static BrowParams  _lbrow_cur = BROW_DEFAULT,   _lbrow_tgt = BROW_DEFAULT;
static BrowParams  _rbrow_cur = BROW_DEFAULT,   _rbrow_tgt = BROW_DEFAULT;
static MouthParams _mouth_cur = MOUTH_DEFAULT,  _mouth_tgt = MOUTH_DEFAULT;

static bool     _blinking   = false;
static float    _blink_t    = 0.0f;
static uint32_t _next_blink = 0;

static bool     _alert_active = false;
static char     _alert_msg[24] = {0};
static uint32_t _alert_expire  = 0;

// =============================================================================
static float flerp(float a, float b, float t) { return a + (b - a) * t; }

static void lerp_params(EyeParams& cur, const EyeParams& tgt, float speed, float dt) {
    float t = 1.0f - expf(-speed * dt);
    cur.outer_r   = flerp(cur.outer_r,   tgt.outer_r,   t);
    cur.iris_r    = flerp(cur.iris_r,    tgt.iris_r,    t);
    cur.pupil_r   = flerp(cur.pupil_r,   tgt.pupil_r,   t);
    cur.lid_top   = flerp(cur.lid_top,   tgt.lid_top,   t);
    cur.lid_bot   = flerp(cur.lid_bot,   tgt.lid_bot,   t);
    cur.look_dx   = flerp(cur.look_dx,   tgt.look_dx,   t);
    cur.look_dy   = flerp(cur.look_dy,   tgt.look_dy,   t);
    cur.lid_slant = flerp(cur.lid_slant, tgt.lid_slant, t);
    cur.iris_color = tgt.iris_color;
}
static void lerp_brow(BrowParams& cur, const BrowParams& tgt, float speed, float dt) {
    float t = 1.0f - expf(-speed * dt);
    cur.inner_y = flerp(cur.inner_y, tgt.inner_y, t);
    cur.outer_y = flerp(cur.outer_y, tgt.outer_y, t);
    cur.raise   = flerp(cur.raise,   tgt.raise,   t);
}
static void lerp_mouth(MouthParams& cur, const MouthParams& tgt, float speed, float dt) {
    float t = 1.0f - expf(-speed * dt);
    cur.smile  = flerp(cur.smile,  tgt.smile,  t);
    cur.open_h = flerp(cur.open_h, tgt.open_h, t);
}

static void _apply_state(FaceState s) {
    EyeParams   l  = PARAMS_DEFAULT;
    EyeParams   r  = PARAMS_DEFAULT;
    BrowParams  lb = BROW_DEFAULT;
    BrowParams  rb = BROW_DEFAULT;
    MouthParams m  = MOUTH_DEFAULT;

    switch (s) {
        case FaceState::IDLE: break;
        case FaceState::THINKING:
            l.look_dx = -7;  l.look_dy = -8;
            r.look_dx = -7;  r.look_dy = -8;
            l.lid_top = 11;
            lb.inner_y = -5; lb.outer_y = 3;
            m.smile = -3;
            break;
        case FaceState::LISTENING:
            l.outer_r = EYE_OUTER_R + 5;  r.outer_r = EYE_OUTER_R + 5;
            l.iris_r  = EYE_IRIS_R  + 3;  r.iris_r  = EYE_IRIS_R  + 3;
            lb.raise = -3; rb.raise = -3;
            break;
        case FaceState::HAPPY:
            l.lid_bot = 4; r.lid_bot = 4;
            lb.raise = -5; lb.inner_y = -2; lb.outer_y = -2;
            rb.raise = -5; rb.inner_y = -2; rb.outer_y = -2;
            m.smile = 15;
            break;
        case FaceState::SAD:
            l.lid_top = 14; r.lid_top = 14;
            l.look_dy = 9;  r.look_dy = 9;
            // iris keeps the accent color — the mood comes from the lid/brow/mouth shape.
            lb.inner_y = -7; lb.outer_y = 4;
            rb.inner_y = -7; rb.outer_y = 4;
            m.smile = -11;
            break;
        case FaceState::SURPRISED:
            l.outer_r = EYE_OUTER_R + 8;  r.outer_r = EYE_OUTER_R + 8;
            l.iris_r  = EYE_IRIS_R  + 5;  r.iris_r  = EYE_IRIS_R  + 5;
            l.pupil_r = EYE_PUPIL_R - 3;  r.pupil_r = EYE_PUPIL_R - 3;
            lb.raise = -9; rb.raise = -9;
            m.open_h = 7;
            break;
        case FaceState::SLEEPING:
            l.lid_top = (float)EYE_OUTER_R * 0.6f;  r.lid_top = l.lid_top;
            l.lid_bot = (float)EYE_OUTER_R * 0.4f;  r.lid_bot = l.lid_bot;
            // iris keeps the accent color.
            lb.raise = 5; lb.inner_y = 3; lb.outer_y = 3;
            rb.raise = 5; rb.inner_y = 3; rb.outer_y = 3;
            m.smile = -4;
            break;
        case FaceState::SPEAKING: break;
        case FaceState::ANGRY:
            l.lid_top   = 12; r.lid_top   = 12;
            l.lid_slant =  9; r.lid_slant = -9;
            l.iris_color = C_DRED; r.iris_color = C_DRED;
            l.outer_r = EYE_OUTER_R - 2; r.outer_r = EYE_OUTER_R - 2;
            lb.inner_y = 7; lb.outer_y = -3;
            rb.inner_y = 7; rb.outer_y = -3;
            m.smile = -8;
            break;
        default: break;
    }
    _left_tgt  = l;  _right_tgt = r;
    _lbrow_tgt = lb; _rbrow_tgt = rb;
    _mouth_tgt = m;
}

static void _draw_eye(int cx, int cy, const EyeParams& p, bool is_happy) {
    int or_ = (int)p.outer_r;
    int ir  = (int)p.iris_r;
    int pr  = (int)p.pupil_r;
    int dx  = (int)p.look_dx;
    int dy  = (int)p.look_dy;

    if (is_happy) {
        _canvas.fillCircle(cx, cy, or_, C_CYAN);
        _canvas.fillRect(cx - or_, cy, or_ * 2, or_ + 2, C_BLACK);
        _canvas.fillCircle(cx + dx + 4, cy + dy - 7, EYE_HLIGHT_R, C_WHITE);
        return;
    }

    _canvas.fillCircle(cx, cy, or_, C_DCYAN);
    _canvas.fillCircle(cx + dx, cy + dy, ir, p.iris_color);
    _canvas.fillCircle(cx + dx, cy + dy, pr, C_BLACK);
    _canvas.fillCircle(cx + dx + 5, cy + dy - 5, EYE_HLIGHT_R, C_WHITE);
    _canvas.fillCircle(cx + dx + 8, cy + dy - 2, 2, C_WHITE);

    int lid_top = (int)p.lid_top;
    if (lid_top > 0) {
        int slant = (int)p.lid_slant;
        if (slant == 0) {
            _canvas.fillRect(cx - or_ - 1, cy - or_ - 1, or_ * 2 + 2, lid_top + 1, C_BLACK);
        } else {
            int abs_sl = abs(slant);
            bool droop_inner = (slant > 0);
            int yl = (cy - or_) + (droop_inner ? abs_sl : 0);
            int yr = (cy - or_) + (droop_inner ? 0 : abs_sl);
            for (int y = (cy - or_) - 1; y < (cy - or_) + lid_top + abs_sl; y++) {
                float ey = flerp((float)yl, (float)yr, 0.5f);
                if (y <= (int)ey + lid_top)
                    _canvas.drawFastHLine(cx - or_ - 1, y, or_ * 2 + 2, C_BLACK);
            }
        }
    }

    int lid_bot = (int)p.lid_bot;
    if (lid_bot > 0)
        _canvas.fillRect(cx - or_ - 1, cy + or_ - lid_bot, or_ * 2 + 2, lid_bot + 2, C_BLACK);

    _canvas.fillCircle(cx - or_, cy - or_, or_ / 2, C_BLACK);
    _canvas.fillCircle(cx + or_, cy - or_, or_ / 2, C_BLACK);
    _canvas.fillCircle(cx - or_, cy + or_, or_ / 2, C_BLACK);
    _canvas.fillCircle(cx + or_, cy + or_, or_ / 2, C_BLACK);
}

static void _draw_brow(int eye_cx, const BrowParams& b, bool is_left) {
    int y_base  = BROW_Y_BASE + (int)b.raise;
    int x_inner = is_left ? eye_cx + BROW_HALF_W : eye_cx - BROW_HALF_W;
    int x_outer = is_left ? eye_cx - BROW_HALF_W : eye_cx + BROW_HALF_W;
    int y_inner = y_base + (int)b.inner_y;
    int y_outer = y_base + (int)b.outer_y;
    int half    = BROW_THICK / 2;
    float span  = (float)(x_inner - x_outer);

    for (int x = min(x_inner, x_outer); x <= max(x_inner, x_outer); x++) {
        float frac = (span != 0.0f) ? (float)(x - x_outer) / span : 0.5f;
        int   ym   = y_outer + (int)(frac * (float)(y_inner - y_outer));
        _canvas.drawFastVLine(x, ym - half, BROW_THICK, C_CYAN);
    }
}

static void _draw_mouth(float smile, float open_h) {
    int hl = MOUTH_THICK / 2;
    for (int x = -MOUTH_HW; x <= MOUTH_HW; x++) {
        float n  = (float)x / (float)MOUTH_HW;
        int   yb = MOUTH_Y + (int)(smile * (1.0f - n * n));
        if (open_h < 2.0f) {
            _canvas.drawFastVLine(SCREEN_CX + x, yb - hl, MOUTH_THICK, C_CYAN);
        } else {
            int gap = (int)(open_h * 0.5f);
            _canvas.drawFastVLine(SCREEN_CX + x, yb - gap - hl, MOUTH_THICK, C_CYAN);
            _canvas.drawFastVLine(SCREEN_CX + x, yb + gap - hl, MOUTH_THICK, C_CYAN);
        }
    }
}

static void _draw_zzz(float t) {
    int ox = R_EYE_CX + 20 + (int)(10 * sinf(t * 0.5f));
    int oy = EYE_CY - 30 - (int)(15 * fmodf(t * 0.3f, 1.0f));
    _canvas.setTextColor(C_CYAN);
    _canvas.setTextSize(2); _canvas.setCursor(ox,      oy);      _canvas.print("z");
    _canvas.setTextSize(3); _canvas.setCursor(ox +  7, oy - 11); _canvas.print("z");
    _canvas.setTextSize(4); _canvas.setCursor(ox + 16, oy - 25); _canvas.print("Z");
}

static void _draw_pulse_ring(int cx, int cy, float t) {
    float pulse = 0.5f + 0.5f * sinf(t * 4.0f);
    int r = (int)(_left_cur.outer_r + 3 + 4 * pulse);
    for (int i = 0; i < 3; i++) _canvas.drawCircle(cx, cy, r + i, C_CYAN);
}

static void _draw_alert() {
    _canvas.fillRoundRect(ALERT_X, ALERT_Y, ALERT_W, ALERT_H, ALERT_R, C_DKCYAN);
    _canvas.drawRoundRect(ALERT_X, ALERT_Y, ALERT_W, ALERT_H, ALERT_R, C_CYAN);
    _canvas.setTextSize(2);
    _canvas.setTextColor(C_WHITE);
    int16_t tw = _canvas.textWidth(_alert_msg);
    int16_t tx = SCREEN_CX - tw / 2;
    if (tx < ALERT_X + 4) tx = ALERT_X + 4;
    _canvas.setCursor(tx, ALERT_Y + 8);
    _canvas.print(_alert_msg);
}

static float _blink_lid(float bt) {
    float phase = bt < 1.0f ? bt : 2.0f - bt;
    return sinf(phase * (float)M_PI) * (float)EYE_OUTER_R;
}

// =============================================================================
// Public API
// =============================================================================
FaceState face_state_from_string(const char* s) {
    if (strcasecmp(s, "thinking")  == 0) return FaceState::THINKING;
    if (strcasecmp(s, "listening") == 0) return FaceState::LISTENING;
    if (strcasecmp(s, "happy")     == 0) return FaceState::HAPPY;
    if (strcasecmp(s, "sad")       == 0) return FaceState::SAD;
    if (strcasecmp(s, "surprised") == 0) return FaceState::SURPRISED;
    if (strcasecmp(s, "sleeping")  == 0) return FaceState::SLEEPING;
    if (strcasecmp(s, "speaking")  == 0) return FaceState::SPEAKING;
    if (strcasecmp(s, "angry")     == 0) return FaceState::ANGRY;
    if (strcasecmp(s, "calibrate") == 0) return FaceState::CALIBRATE;
    return FaceState::IDLE;
}

const char* face_state_to_string(FaceState s) {
    switch (s) {
        case FaceState::THINKING:  return "thinking";
        case FaceState::LISTENING: return "listening";
        case FaceState::HAPPY:     return "happy";
        case FaceState::SAD:       return "sad";
        case FaceState::SURPRISED: return "surprised";
        case FaceState::SLEEPING:  return "sleeping";
        case FaceState::SPEAKING:  return "speaking";
        case FaceState::ANGRY:     return "angry";
        case FaceState::CALIBRATE: return "calibrate";
        default:                   return "idle";
    }
}

void face_init(lgfx::LGFX_Device* lcd) {
    _lcd = lcd;
    if (_lcd) _lcd->setRotation(0);
    _canvas.setPsram(false);
    _canvas.setColorDepth(lgfx::rgb565_2Byte);
    _canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (_canvas.getBuffer() == nullptr) {
        Serial.println("[FACE] FATAL: sprite allocation failed (need ~115 KB SRAM)");
        while (true) delay(1000);
    }
    _last_ms    = millis();
    _next_blink = millis() + 3000 + random(2000);
    _state_ms   = millis();
    Serial.println("[FACE] Animation engine ready (240x240 SRAM sprite)");
}

void face_set_state(FaceState s) {
    if (s == _state) return;
    if (s == FaceState::CALIBRATE)              imu_lock_rotation(0);
    else if (_state == FaceState::CALIBRATE)    imu_lock_rotation(-1);
    _state    = s;
    _state_ms = millis();
    _blinking = false;
    _blink_t  = 0.0f;
    _apply_state(s);
    Serial.printf("[FACE] State -> %s\n", face_state_to_string(s));
}

FaceState face_get_state() { return _state; }

void face_show_alert(const char* raw, uint32_t default_duration_ms) {
    char buf[32];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint32_t dur = default_duration_ms;
    char* pipe = strchr(buf, '|');
    if (pipe) {
        *pipe = '\0';
        uint32_t d = (uint32_t)atol(pipe + 1);
        if (d >= 500 && d <= 60000) dur = d;
    }

    strncpy(_alert_msg, buf, sizeof(_alert_msg) - 1);
    _alert_msg[sizeof(_alert_msg) - 1] = '\0';
    if (strlen(_alert_msg) > 14) {
        _alert_msg[12] = '.'; _alert_msg[13] = '.'; _alert_msg[14] = '\0';
    }

    _alert_expire = millis() + dur;
    _alert_active = true;
    Serial.printf("[FACE] Alert: \"%s\" (%lu ms)\n", _alert_msg, dur);
}

void face_dismiss_alert() { _alert_active = false; }
bool face_alert_active()  { return _alert_active; }

void face_set_brightness(uint8_t v) { if (_lcd) _lcd->setBrightness(v); }

void face_set_rotation(uint8_t r) {
    _smooth_angle  = (float)(r & 3) * 90.0f;
    _rotation_auto = false;
}

void face_set_rotation_auto(bool enable) {
    // Snap the smoothed angle to the current physical orientation when
    // auto-rotate is (re-)enabled. Without this, _smooth_angle holds whatever
    // the previous sketch/face left it at and the slew filter takes ~1 second
    // to reach the true orientation — sketches that call autoRotate() in
    // setup() visibly start upside-down then flip around as they converge.
    if (enable && imu_is_ok()) {
        float ax = imu_get_ax();
        float ay = imu_get_ay();
        _smooth_angle = atan2f(ax, -ay) * (180.0f / (float)M_PI) + 90.0f;
    }
    _rotation_auto = enable;
}

void face_set_accent(uint8_t r, uint8_t g, uint8_t b) {
    _accent_r = r; _accent_g = g; _accent_b = b;
    C_CYAN   = _rgb565(r, g, b);
    // 40 % shade for the eye outer ring, 10 % for the alert chrome.
    C_DCYAN  = _rgb565((uint8_t)(r * 4 / 10), (uint8_t)(g * 4 / 10), (uint8_t)(b * 4 / 10));
    C_DKCYAN = _rgb565((uint8_t)(r / 10),     (uint8_t)(g / 10),     (uint8_t)(b / 10));
    PARAMS_DEFAULT.iris_color = C_CYAN;
    _apply_state(_state);   // re-build targets so the new color lerps in
    Serial.printf("[FACE] accent -> (%u,%u,%u) #%02X%02X%02X\n", r, g, b, r, g, b);
}
uint8_t face_get_accent_r() { return _accent_r; }
uint8_t face_get_accent_g() { return _accent_g; }
uint8_t face_get_accent_b() { return _accent_b; }

// =============================================================================
// Shared with p5 bindings — single canvas + shared rotation smoother.
// =============================================================================
LGFX_Sprite*       face_get_canvas()         { return &_canvas; }
lgfx::LGFX_Device* face_get_lcd()            { return _lcd; }
float              face_get_rotation_angle() { return _smooth_angle; }

void face_update_rotation_smoothing(float dt) {
    if (!_rotation_auto || !imu_is_ok()) return;
    float ax = imu_get_ax();
    float ay = imu_get_ay();
    float target = atan2f(ax, -ay) * (180.0f / (float)M_PI) + 90.0f;
    float diff = target - _smooth_angle;
    while (diff >  180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    _smooth_angle += diff * 4.0f * dt;
}

// =============================================================================
// face_tick — render one fallback-face frame
// =============================================================================
void face_tick() {
    uint32_t now = millis();
    float dt = (float)(now - _last_ms) / 1000.0f;
    if (dt <= 0) dt = 0.001f;
    _last_ms = now;
    float t = (float)(now - _state_ms) / 1000.0f;

    // The fallback face always auto-rotates. Sketches may have left
    // _rotation_auto disabled; restore it whenever the face is the renderer.
    // CALIBRATE intentionally locks rotation so the IMU debug view stays put.
    if (_state != FaceState::CALIBRATE) _rotation_auto = true;

    if (_state == FaceState::CALIBRATE) {
        float ax = imu_get_ax();
        float ay = imu_get_ay();
        const float SCALE = 55.0f;
        int tip_x = SCREEN_CX - (int)(ay * SCALE);
        int tip_y = SCREEN_CY + (int)(ax * SCALE);

        _canvas.fillScreen(C_BLACK);
        _canvas.drawLine(SCREEN_CX - 10, SCREEN_CY, SCREEN_CX + 10, SCREEN_CY, C_WHITE);
        _canvas.drawLine(SCREEN_CX, SCREEN_CY - 10, SCREEN_CX, SCREEN_CY + 10, C_WHITE);
        _canvas.drawLine(SCREEN_CX,     SCREEN_CY,     tip_x,     tip_y,     C_CYAN);
        _canvas.drawLine(SCREEN_CX + 1, SCREEN_CY,     tip_x + 1, tip_y,     C_CYAN);
        _canvas.drawLine(SCREEN_CX,     SCREEN_CY + 1, tip_x,     tip_y + 1, C_CYAN);
        _canvas.drawLine(SCREEN_CX - 1, SCREEN_CY,     tip_x - 1, tip_y,     C_CYAN);
        _canvas.fillCircle(tip_x, tip_y, 5, C_CYAN);
        _canvas.fillCircle(SCREEN_CX, SCREEN_CY, 3, C_WHITE);

        _canvas.setTextColor(C_WHITE);
        _canvas.setTextSize(2);
        char buf[24];
        snprintf(buf, sizeof(buf), "ax=%.2f", ax);
        _canvas.setCursor(SCREEN_CX - 36, SCREEN_CY + 30); _canvas.print(buf);
        snprintf(buf, sizeof(buf), "ay=%.2f", ay);
        _canvas.setCursor(SCREEN_CX - 36, SCREEN_CY + 47); _canvas.print(buf);
        const float T = 0.6f;
        int would_rot = 0;
        if      (ay < -T) would_rot = 0;
        else if (ax >  T) would_rot = 1;
        else if (ay >  T) would_rot = 2;
        else if (ax < -T) would_rot = 3;
        snprintf(buf, sizeof(buf), "rot=%d", would_rot);
        _canvas.setCursor(SCREEN_CX - 20, SCREEN_CY + 65); _canvas.print(buf);

        _canvas.pushSprite(_lcd, 0, 0);
        return;
    }

    lerp_params(_left_cur,  _left_tgt,  6.0f, dt);
    lerp_params(_right_cur, _right_tgt, 6.0f, dt);
    lerp_brow(_lbrow_cur, _lbrow_tgt, 6.0f, dt);
    lerp_brow(_rbrow_cur, _rbrow_tgt, 6.0f, dt);
    lerp_mouth(_mouth_cur, _mouth_tgt, 5.0f, dt);

    if (_alert_active && now > _alert_expire) _alert_active = false;

    float blink_lid_offset = 0.0f;
    if (_state == FaceState::IDLE || _state == FaceState::LISTENING) {
        if (!_blinking && now >= _next_blink) { _blinking = true; _blink_t = 0.0f; }
        if (_blinking) {
            _blink_t += dt * 7.0f;
            if (_blink_t >= 2.0f) {
                _blinking = false; _blink_t = 0.0f;
                _next_blink = now + 3000 + random(2500);
            } else {
                blink_lid_offset = _blink_lid(_blink_t);
            }
        }
    }

    int happy_bounce = (_state == FaceState::HAPPY) ? (int)(2 * sinf(t * 4.0f)) : 0;

    uint16_t bg = C_BLACK;
    if (_state == FaceState::SURPRISED && t < 0.15f) {
        uint8_t flash = (uint8_t)(180 * (1.0f - t / 0.15f));
        bg = _canvas.color888(flash, flash, flash);
    }

    int jx = 0, jy = 0;
    if (_state == FaceState::ANGRY && t > 0.3f) {
        jx = (int)(2 * sinf(t * 23.7f));
        jy = (int)(1 * cosf(t * 17.3f));
    }

    MouthParams md = _mouth_cur;
    if (_state == FaceState::SPEAKING)
        md.open_h = 4.0f + 7.0f * fabsf(sinf(t * 8.0f));

    _canvas.fillScreen(bg);
    bool is_happy = (_state == FaceState::HAPPY);

    EyeParams ld = _left_cur, rd = _right_cur;
    ld.lid_top += blink_lid_offset;
    rd.lid_top += blink_lid_offset;

    _draw_eye(L_EYE_CX + jx, EYE_CY + happy_bounce + jy, ld, is_happy);
    _draw_eye(R_EYE_CX + jx, EYE_CY + happy_bounce + jy, rd, is_happy);

    BrowParams lb = _lbrow_cur, rb = _rbrow_cur;
    if (_state == FaceState::ANGRY) { lb.raise += jy; rb.raise += jy; }
    _draw_brow(L_EYE_CX + jx, lb, true);
    _draw_brow(R_EYE_CX + jx, rb, false);

    _draw_mouth(md.smile, md.open_h);

    if (_state == FaceState::LISTENING) {
        _draw_pulse_ring(L_EYE_CX, EYE_CY, t);
        _draw_pulse_ring(R_EYE_CX, EYE_CY, t);
    }
    if (_state == FaceState::SLEEPING) _draw_zzz(t);
    if (_alert_active) _draw_alert();

    face_update_rotation_smoothing(dt);

    _canvas.pushRotateZoom(_lcd, SCREEN_CX, SCREEN_CY, _smooth_angle, 1.0f, 1.0f);
}
