/*
 * input.c - Wiimote (botones + puntero IR), Classic, mando de GameCube (stick =
 * puntero virtual) y teclado USB unificados en bits WPAD_BUTTON_*.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/keyboard.h>
#include <ogc/lwp.h>
#include <math.h>
#include "tv.h"
#include "guard.h"

static int kb_ok = 0;
static int keyq[64], kq_head = 0, kq_tail = 0;
static u32 inj_btn = 0;
static int rep_cnt = 0;
static u32 rep_btn = 0;
static u64 rep_t0 = 0, rep_last = 0;
in_ptr_t in_ptr = { 320, 240, 0, 0, 0, 0 };
static float vx = 320, vy = 240;
static u64 vlast = 0;
static int forced = 0;

void input_init(void) {
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_ALL, 640, 480);
    PAD_Init();
    kb_ok = KEYBOARD_Init(NULL) >= 0;
}

static void push_key(int k) {
    int n = (kq_tail + 1) % 64;
    if (n != kq_head) { keyq[kq_tail] = k; kq_tail = n; }
}
void in_inject_key(int k) { push_key(k); }
void in_inject_btn(u32 b) { inj_btn |= b; }

static u32 map_classic(u32 c) {
    u32 d = c & 0xFFFF;
    if (c & WPAD_CLASSIC_BUTTON_A) d |= WPAD_BUTTON_A;
    if (c & WPAD_CLASSIC_BUTTON_B) d |= WPAD_BUTTON_B;
    if (c & WPAD_CLASSIC_BUTTON_UP) d |= WPAD_BUTTON_UP;
    if (c & WPAD_CLASSIC_BUTTON_DOWN) d |= WPAD_BUTTON_DOWN;
    if (c & WPAD_CLASSIC_BUTTON_LEFT) d |= WPAD_BUTTON_LEFT;
    if (c & WPAD_CLASSIC_BUTTON_RIGHT) d |= WPAD_BUTTON_RIGHT;
    if (c & WPAD_CLASSIC_BUTTON_PLUS) d |= WPAD_BUTTON_PLUS;
    if (c & WPAD_CLASSIC_BUTTON_MINUS) d |= WPAD_BUTTON_MINUS;
    if (c & WPAD_CLASSIC_BUTTON_HOME) d |= WPAD_BUTTON_HOME;
    if (c & WPAD_CLASSIC_BUTTON_X) d |= WPAD_BUTTON_1;
    if (c & WPAD_CLASSIC_BUTTON_Y) d |= WPAD_BUTTON_2;
    return d & 0xFFFF;
}
static u32 map_gc(u16 g) {
    u32 d = 0;
    if (g & PAD_BUTTON_A) d |= WPAD_BUTTON_A;
    if (g & PAD_BUTTON_B) d |= WPAD_BUTTON_B;
    if (g & PAD_BUTTON_UP) d |= WPAD_BUTTON_UP;
    if (g & PAD_BUTTON_DOWN) d |= WPAD_BUTTON_DOWN;
    if (g & PAD_BUTTON_LEFT) d |= WPAD_BUTTON_LEFT;
    if (g & PAD_BUTTON_RIGHT) d |= WPAD_BUTTON_RIGHT;
    if (g & PAD_BUTTON_START) d |= WPAD_BUTTON_PLUS;
    if (g & PAD_TRIGGER_Z) d |= WPAD_BUTTON_MINUS;
    if (g & PAD_BUTTON_X) d |= WPAD_BUTTON_1;
    if (g & PAD_BUTTON_Y) d |= WPAD_BUTTON_2;
    return d;
}

static lwp_t input_owner = LWP_THREAD_NULL;
u32 in_poll(void) {
    if (input_owner == LWP_THREAD_NULL) input_owner = LWP_GetSelf();
    if (LWP_GetSelf() != input_owner) return 0;
    guard_beat();
    u32 d, held;
    u64 now;
    WPAD_ScanPads();
    PAD_ScanPads();
    d = map_classic(WPAD_ButtonsDown(0)) | map_gc(PAD_ButtonsDown(0));
    held = map_classic(WPAD_ButtonsHeld(0)) | map_gc(PAD_ButtonsHeld(0));
    d |= inj_btn; inj_btn = 0;
    now = gettime();

    /* autorepeticion de la cruceta (400 ms, luego cada 90 ms) */
    u32 dirs = held & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT);
    if (dirs != rep_btn) { rep_btn = dirs; rep_t0 = rep_last = now; rep_cnt = 0; }
    else if (dirs && ticks_to_millisecs(now - rep_t0) > 400 && ticks_to_millisecs(now - rep_last) > 90) {
        d |= dirs; rep_last = now; rep_cnt++;
    }

    /* puntero: IR del Wiimote, o stick del mando de GameCube / Classic */
    WPADData *wd = WPAD_Data(0);
    float dt = vlast ? ticks_to_millisecs(now - vlast) / 16.7f : 1; vlast = now;
    if (dt > 4) dt = 4;
    if (forced) {
        /* puntero fijado por la prueba automatica */
    } else if (wd && wd->ir.valid) {
        /* rc12: el IR esta en coordenadas del XFB; se pasa al lienzo de la zona segura. */
        vx = gfx_pointer_x((int)wd->ir.x); vy = gfx_pointer_y((int)wd->ir.y);
        in_ptr.valid = wd->ir.x >= 0 && wd->ir.x < FB_W; in_ptr.idle = 0;
    } else {
        int sx = PAD_StickX(0), sy = PAD_StickY(0);
        /* Classic left stick: same virtual pointer as the GameCube stick (0 deg = up, 90 = right). */
        if (wd && wd->exp.type == WPAD_EXP_CLASSIC && wd->exp.classic.ljs.mag > 0.15f) {
            float a = wd->exp.classic.ljs.ang * (3.14159265f / 180.f);
            sx = (int)(wd->exp.classic.ljs.mag * sinf(a) * 128.f);
            sy = (int)(wd->exp.classic.ljs.mag * cosf(a) * 128.f);
        }
        if (sx * sx + sy * sy > 20 * 20) {
            vx += sx * 0.09f * dt; vy -= sy * 0.09f * dt;
            in_ptr.valid = 1; in_ptr.idle = 0;
        } else if (in_ptr.valid && in_ptr.virt) {
            if (++in_ptr.idle > 240) in_ptr.valid = 0;   /* se oculta tras ~4 s sin moverse */
        } else in_ptr.valid = in_ptr.virt ? in_ptr.valid : 0;
        in_ptr.virt = 1;
    }
    if (!forced && wd && wd->ir.valid) in_ptr.virt = 0;
    if (vx < 0) vx = 0; if (vx >= SCR_W) vx = SCR_W-1;
    if (vy < 0) vy = 0; if (vy > 479) vy = 479;
    in_ptr.x = (int)vx; in_ptr.y = (int)vy;
    in_ptr.held = held | (d & WPAD_BUTTON_A);

    if (kb_ok) {
        keyboard_event ev;
        while (KEYBOARD_GetEvent(&ev) > 0) {
            if (ev.type != KEYBOARD_PRESSED) continue;
            int s = ev.symbol;
            if (s == 0x08 || s == 0x7F) s = 8;
            else if (s == 0x0D || s == 0x0A) s = 13;
            else if (s == 0x1B) s = 27;
            else if (s < 32 || s > 126) continue;
            push_key(s);
        }
    }
#ifdef AUTOTEST
    d |= at_hook();
#endif
    return d;
}

/* posicion forzada del puntero (pruebas automaticas) */
void in_set_ptr(int x, int y, int valid) { vx = x; vy = y; in_ptr.x = x; in_ptr.y = y; in_ptr.valid = valid; in_ptr.virt = 1; in_ptr.idle = 0; forced = 1; }

int in_key(void) {
    if (kq_head == kq_tail) return 0;
    int k = keyq[kq_head];
    kq_head = (kq_head + 1) % 64;
    return k;
}
