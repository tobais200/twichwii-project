/*
 * ui.c - capa de interfaz grafica: botones, paneles, puntero, teclado en
 * pantalla, listas, visor de texto, confirmaciones y pantalla de espera.
 * Estilo oscuro de twitch.tv con acento violeta #9146FF, apto para el puntero (TwitchWii, no oficial).
 * Todo el arte es propio (primitivas de gfx.c).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp.h>
#include "tv.h"
#include "guard.h"
#include "mem2.h"
#define KB_MEM2_ALLOC twitchwii_mem2_alloc

int ui_quit = 0;
ui_frame_t UI;
static void (*backdrop_fn)(void) = NULL;
static lwp_t ui_owner = LWP_THREAD_NULL;
static int backdrop_suspended = 0;
void ui_backdrop_suspend(int on) { backdrop_suspended = on != 0; }
static int busy_on = 0;
static char busy_title[80], busy_msg[220];
static u64 busy_last = 0;
static int anim = 0;

void ui_set_backdrop(void (*fn)(void)) {
    if (ui_owner == LWP_THREAD_NULL) ui_owner = LWP_GetSelf();
    backdrop_fn = fn;
}

void ui_begin(u32 down) { UI.down = down; UI.consumed = 0; busy_on = 0; anim++; }

int ui_hover(int x, int y, int w, int h) {
    return in_ptr.valid && in_ptr.x >= x && in_ptr.x < x + w && in_ptr.y >= y && in_ptr.y < y + h;
}

int ui_click(int x, int y, int w, int h) {
    if (UI.consumed || !(UI.down & WPAD_BUTTON_A) || !ui_hover(x, y, w, h)) return 0;
    UI.consumed = 1;
    return 1;
}

int ui_button(int x, int y, int w, int h, const char *label, int icon, int style, int focus) {
    int hov = ui_hover(x, y, w, h);
    int r = h / 2 > 10 ? 10 : h / 2;
    u32 bg = C_PANEL2, fg = C_TEXT;
    if (style == BS_ACCENT) bg = C_ACCENT;
    if (style == BS_GHOST) bg = C_PANEL;
    if (style == BS_ICON) r = h / 2;
    if (style == BS_ACCENT) fg = 0xFFFFFF;
    if (hov || focus) bg = style == BS_ACCENT ? C_ACCENT_HI : gfx_mix(bg, 0xFFFFFF, 40);
    if (hov && (in_ptr.held & WPAD_BUTTON_A)) { y += 1; }
    gfx_rrect(x, y, w, h, r, bg, 255);
    if (focus) gfx_rframe(x - 2, y - 2, w + 4, h + 4, r + 2, 2, style == BS_ACCENT ? 0xFFFFFF : C_ACCENT, 255);
    int tw = label && label[0] ? gfx_text_w(&font_bold, label) : 0;
    int isz = h * 9 / 20;
    int total = tw + (icon ? isz + (tw ? 8 : 0) : 0);
    int px = x + (w - total) / 2;
    if (icon) { gfx_icon(icon, px + isz / 2, y + h / 2, isz, fg); px += isz + 8; }
    if (tw) gfx_text(&font_bold, px, y + (h - font_bold.line_h) / 2 + 1, label, fg);
    return ui_click(x, y, w, h);
}

void ui_logo(int x, int y, int s) {
    /* icono propio (no es el logo oficial): globo de chat violeta con una "pantalla"
       de Wii y dos barritas, hecho con primitivas */
    int r = s / 6;
    gfx_rrect(x, y, s, s * 82 / 100, r, C_ACCENT, 255);
    gfx_tri(x + s * 22 / 100, y + s * 80 / 100, x + s * 46 / 100, y + s * 80 / 100, x + s * 22 / 100, y + s, C_ACCENT);
    gfx_rrect(x + s * 18 / 100, y + s * 16 / 100, s * 64 / 100, s * 48 / 100, s / 10 + 1, 0xFFFFFF, 255);
    gfx_rect(x + s * 38 / 100, y + s * 27 / 100, s / 10 + 1, s * 26 / 100, C_ACCENT);
    gfx_rect(x + s * 56 / 100, y + s * 27 / 100, s / 10 + 1, s * 26 / 100, C_ACCENT);
}

void ui_draw_cursor(void) {
    if (!in_ptr.valid) return;
    int x = in_ptr.x, y = in_ptr.y;
    /* flecha propia: sombra + borde blanco + relleno de acento */
    gfx_tri(x + 2, y + 3, x + 2, y + 25, x + 18, y + 19, 0x000000);
    gfx_tri(x, y, x, y + 23, x + 17, y + 17, 0xFFFFFF);
    gfx_tri(x + 2, y + 5, x + 2, y + 18, x + 12, y + 15, C_ACCENT);
    gfx_line(x + 6, y + 18, x + 10, y + 26, 4, 0xFFFFFF);
}

void ui_present(void) {
    gfx_noclip();
    ui_draw_cursor();
    gfx_present();
}

void ui_backdrop(int dim) {
    if (backdrop_fn && !backdrop_suspended) {
        u32 save = UI.down; int sc = UI.consumed;
        UI.down = 0; UI.consumed = 1;     /* el fondo no recibe clics */
        backdrop_fn();
        UI.down = save; UI.consumed = sc;
    } else gfx_clear(C_BG);
    gfx_noclip();
    if (dim) gfx_rect_a(0, 0, SCR_W, SCR_H, 0x000000, dim);
}

void ui_panel(int x, int y, int w, int h, const char *title) {
    gfx_rrect(x + 4, y + 6, w, h, 14, 0x000000, 120);
    gfx_rrect(x, y, w, h, 14, C_PANEL, 255);
    gfx_rframe(x, y, w, h, 14, 1, C_LINE, 255);
    if (title) {
        gfx_rrect(x, y, w, 44, 14, C_BG2, 255);
        gfx_rect(x, y + 30, w, 14, C_BG2);
        gfx_rect(x, y + 44, w, 2, C_ACCENT);
        ui_logo(x + 12, y + 9, 26);
        gfx_text_fit(&font_bold, x + 48, y + 13, title, C_TEXT, w - 64);
    }
}

void ui_spinner(int cx, int cy, int r) {
    int i;
    static const signed char ox[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const signed char oy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    int head = (anim / 4) % 8;
    for (i = 0; i < 8; i++) {
        int age = (head - i + 8) % 8;
        gfx_circle(cx + ox[i] * r / 10, cy + oy[i] * r / 10, r / 5 + 1, gfx_mix(C_ACCENT, C_PANEL, age * 30), 255);
    }
}

static char toast_msg[200];
static u64 toast_t = 0;
void ui_toast(const char *msg) { snprintf(toast_msg, sizeof(toast_msg), "%s", msg); toast_t = gettime(); }
void ui_draw_toast(int ybottom) {
    if (!toast_msg[0] || !toast_t) return;
    u32 ms = ticks_to_millisecs(gettime() - toast_t);
    if (ms > 4300) return;
    int a = ms > 4000 ? (4300 - ms) * 255 / 300 : 255;
    int w = gfx_text_w(&font_body, toast_msg) + 32;
    if (w > 560) w = 560;
    int x = (SCR_W - w) / 2, y = ybottom - 40;
    gfx_rrect(x, y, w, 32, 16, 0x2C2C36, a * 235 / 255);
    gfx_rect_a(x + 12, y + 9, 3, 14, C_ACCENT, a);
    gfx_text_fit(&font_body, x + 22, y + 8, toast_msg, gfx_mix(0x2C2C36, C_TEXT, a), w - 34);
}

/* ---------------- espera ---------------- */
static void busy_draw(void) {
    ui_backdrop(150);
    int w = 420, h = 150, x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
    ui_panel(x, y, w, h, busy_title);
    ui_spinner(x + 44, y + 92, 16);
    gfx_text_fit(&font_body, x + 78, y + 72, busy_msg, C_TEXT, w - 96);
    gfx_text(&font_small, x + 78, y + 100, "HOME = cancelar", C_FAINT);
    ui_present();
    busy_last = gettime();
}

void sui_busy(const char *title, const char *msg) {
    snprintf(busy_title, sizeof(busy_title), "%s", title);
    snprintf(busy_msg, sizeof(busy_msg), "%s", msg);
    busy_on = 1;
    busy_draw();
    thumbs_pause(1);
}
void sui_busy_end(void) { busy_on = 0; thumbs_pause(0); }

void ui_busy_tick(void) {
    if (LWP_GetSelf() != ui_owner) return;
    guard_beat();
    if (!busy_on) return;
    if (ticks_to_millisecs(gettime() - busy_last) < 66) return;
    anim += 3;
    busy_draw();
}

static void wait_release(void) {
    int i;
    for (i = 0; i < 2; i++) { in_poll(); VIDEO_WaitVSync(); }
}

/* ---------------- teclado en pantalla ---------------- */
#define KB_ROWS 4
static const char *kb_rows[KB_ROWS] = { "1234567890", "qwertyuiop", "asdfghjkl\xf1", "zxcvbnm-_." };
enum { K_SPACE, K_BACK, K_CLEAR, K_SHIFT, K_CANCEL, K_OK, K_EMOTE, K_NSPECIAL };

static void kb_char(char *buf, int max, int *len, int ch) {
    /* inserta Latin-1 como UTF-8 */
    if (ch < 0x80) { if (*len < max) { buf[(*len)++] = ch; buf[*len] = 0; } }
    else if (*len + 1 < max) { buf[(*len)++] = 0xC0 | (ch >> 6); buf[(*len)++] = 0x80 | (ch & 0x3F); buf[*len] = 0; }
}
/* rc14: selector de emotes en el teclado. La app los fija antes de abrir el teclado
 * (redaccion del chat) y los limpia despues; en las demas pantallas quedan en NULL. */
int (*sui_kb_emote_hook)(char *out, int cap) = NULL;              /* abre la grilla; 1 = out tiene el texto a insertar */
int (*sui_kb_preview)(const char *buf, int x, int y, int w) = NULL; /* dibuja el campo con imagenes; devuelve x del cursor o -1 */
static void kb_back(char *buf, int *len) {
    if (!*len) return;
    if (buf[*len - 1] == ']') {   /* [emote:ID:nombre] de Kick: se borra entero */
        char *t = NULL;
        for (int i = *len - 1; i >= 0; i--) if (!strncmp(buf + i, "[emote:", 7)) { t = buf + i; break; }
        if (t && !strchr(t, ' ') && strchr(t + 7, ':')) { *len = t - buf; buf[*len] = 0; return; }
    }
    (*len)--;
    while (*len > 0 && ((unsigned char)buf[*len] & 0xC0) == 0x80) (*len)--;
    buf[*len] = 0;
}

/* ---- teclado en pantalla: capa estatica cacheada + redibujo de rectangulos sucios ----
 * Antes cada cuadro volvia a dibujar el fondo completo (draw_home/etc.), la capa oscura, el panel,
 * las 45 teclas y reconvertia los 640x480 px a YUYV con filtro bilineal (~50 ms por cuadro = 20 fps,
 * 15 fps en 16:9). Ahora el fondo + panel + marco del campo + ayuda se dibujan UNA vez al abrir y se
 * guardan en una copia (kb_snap). Despues, por cuadro solo se redibujan las teclas cuyo estado cambio
 * (foco / puntero encima / apretada), el campo de texto al escribir o al parpadear el cursor, y la
 * flecha del puntero; solo esas filas/columnas se reconvierten a YUYV (gfx_ui_*) y se copian al XFB.
 * Si no cambia nada no se dibuja ni se presenta nada. Mismo aspecto, mismas teclas y entradas. */
typedef struct { int x0, y0, x1, y1; } kb_rect_t;
static u32 *kb_snap = NULL;
static void kb_rect_add(kb_rect_t *L, int *n, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x1 <= x0 || y1 <= y0) return;
    if (*n >= 16) {   /* demasiados: una sola union */
        for (int i = 1; i < *n; i++) { if (L[i].x0 < L[0].x0) L[0].x0 = L[i].x0; if (L[i].y0 < L[0].y0) L[0].y0 = L[i].y0; if (L[i].x1 > L[0].x1) L[0].x1 = L[i].x1; if (L[i].y1 > L[0].y1) L[0].y1 = L[i].y1; }
        *n = 1;
    }
    L[*n].x0 = x0; L[*n].y0 = y0; L[*n].x1 = x1; L[*n].y1 = y1; (*n)++;
}
static int kb_isect(const kb_rect_t *a, int x0, int y0, int x1, int y1) { return a->x0 < x1 && x0 < a->x1 && a->y0 < y1 && y0 < a->y1; }
static void kb_rects_merge(kb_rect_t *L, int *n) {
    int again = 1;
    while (again) {
        again = 0;
        for (int i = 0; i < *n && !again; i++) for (int j = i + 1; j < *n; j++) {
            if (kb_isect(&L[i], L[j].x0, L[j].y0, L[j].x1, L[j].y1)) {
                if (L[j].x0 < L[i].x0) L[i].x0 = L[j].x0;
                if (L[j].y0 < L[i].y0) L[i].y0 = L[j].y0;
                if (L[j].x1 > L[i].x1) L[i].x1 = L[j].x1;
                if (L[j].y1 > L[i].y1) L[i].y1 = L[j].y1;
                L[j] = L[--(*n)];
                again = 1; break;
            }
        }
    }
}
static void kb_restore(const kb_rect_t *R) {
    for (int y = R->y0; y < R->y1; y++) memcpy(canvas + y * SCR_W + R->x0, kb_snap + y * SCR_W + R->x0, (R->x1 - R->x0) * sizeof(u32));
}
#define KB_CURSOR_RECT(L, n, x, y) kb_rect_add(L, n, (x) - 1, (y) - 1, (x) + 22, (y) + 30)

#ifdef KBD_BENCH
#include <stdarg.h>
extern unsigned gfx_last_conv_us;
static FILE *kbf = NULL;
static void kbl(const char *fmt, ...) {
    if (!kbf) kbf = fopen("sd:/kbd-bench.log", "w");
    if (!kbf) return;
    va_list ap; va_start(ap, fmt); vfprintf(kbf, fmt, ap); va_end(ap); fputc('\n', kbf); fflush(kbf);
}
#define KBT() ((unsigned)ticks_to_microsecs(gettime()))
#endif

int sui_text_input_ex(const char *title, const char *hint, char *buf, int max,const char *accept,const char *placeholder) {
    int r = 1, c = 0, shift = 0, len = strlen(buf), done = -1, i, k;
    int px = (SCR_W-592)/2, py = 40, pw = 592, ph = 404;   /* rc14: 20 px mas: la ayuda ya no pisa la ultima fila */
    static const char *sl[K_NSPECIAL] = { "Espacio", "Borrar", "Limpiar", "Mayus", "Cancelar", "Buscar", "Emotes" };
    static const int sw0[K_NSPECIAL] = { 120, 84, 80, 70, 86, 84, 0 };
    static const int sw1[K_NSPECIAL] = { 68, 80, 72, 64, 80, 80, 74 };   /* con la tecla Emotes: mismo ancho total */
    const int NK = KB_ROWS * 10 + K_NSPECIAL;
    /* geometria fija del teclado */
    int fx = px + 20, fy = py + 76, fw = pw - 40, fh = 44;
    int kw = 50, kh = 42, gap = 6, kx0 = px + (pw - (10 * kw + 9 * gap)) / 2, ky0 = py + 136;
    int spx[K_NSPECIAL] = { 0 };
    {
        int has0 = sui_kb_emote_hook != NULL, nsp0 = has0 ? K_NSPECIAL : K_EMOTE;
        const int *sw = has0 ? sw1 : sw0;
        int sx = kx0;
        for (k = 0; k < nsp0; k++) { if (k == K_EMOTE) sx = kx0 + 10 * kw + 9 * gap - sw[k]; spx[k] = sx; sx += sw[k] + gap; }
    }
    int sy = ky0 + KB_ROWS * (kh + gap) + 4;
    int need_full = 1, have_snap = 0;
    unsigned char kst[KB_ROWS * 10 + K_NSPECIAL];
    unsigned kb_hash = 0; int kb_phase = -1, pcx = -1000, pcy = -1000, pcv = 0, pshift = 0;
    memset(kst, 0xFF, sizeof kst);
#ifdef KBD_BENCH
    unsigned kb_t_open = KBT(), kb_t_prev = kb_t_open, kb_frame = 0, kb_sum_draw[3] = {0}, kb_sum_pres[3] = {0}, kb_sum_conv[3] = {0}, kb_n[3] = {0}, kb_max[3] = {0}, kb_first = 0, kb_present_n[3] = {0}, kb_loop[3] = {0};
    kbl("keyboard open (canvas %d)", SCR_W);
#endif
    if (!kb_snap) kb_snap = KB_MEM2_ALLOC((u32)(SCR_MAX_W * SCR_H * sizeof(u32)), 32);
    in_poll();   /* descarta el flanco del boton que abrio el teclado (antes: 2 cuadros de espera = ~33 ms) */
    while (SYS_MainLoop() && done < 0) {
        u32 d = in_poll();
        int key;
#ifdef KBD_BENCH
        unsigned kb_t0 = KBT();
        if (kb_frame == 90) in_set_ptr(100, 200, 1);
        if (kb_frame > 90 && kb_frame < 100) in_set_ptr(100 + (kb_frame - 90) * 6, 200 + ((kb_frame / 7) % 4) * 48, 1);
        if (kb_frame >= 100 && kb_frame < 140) in_set_ptr(spx[K_SHIFT] + 20, sy + 20, 1);
        if (kb_frame == 104) in_inject_btn(WPAD_BUTTON_A);
        if (kb_frame > 140 && kb_frame < 170) in_set_ptr(100 + (kb_frame - 90) * 6, 200 + ((kb_frame / 7) % 4) * 48, 1);
        if (kb_frame >= 180 && kb_frame < 220 && kb_frame % 8 == 0) in_inject_key("hola mundo"[(kb_frame - 180) / 8 % 10]);
        if (kb_frame == 240) in_inject_key(27);
#endif
        ui_begin(d);
        while ((key = in_key()) != 0) {
            if (key == 13) done = 1;
            else if (key == 27) done = 0;
            else if (key == 8) kb_back(buf, &len);
            else if (key >= 32 && key < 127) kb_char(buf, max, &len, key);
        }
        if (d & WPAD_BUTTON_HOME) done = 0;
        if (d & WPAD_BUTTON_UP)    r = (r + KB_ROWS) % (KB_ROWS + 1);
        if (d & WPAD_BUTTON_DOWN)  r = (r + 1) % (KB_ROWS + 1);
        int has_emo = sui_kb_emote_hook != NULL, nsp = has_emo ? K_NSPECIAL : K_EMOTE;
        const int *sw = has_emo ? sw1 : sw0;
        int cols = (r == KB_ROWS) ? nsp : 10;
        if (c >= cols) c = cols - 1;
        if (d & WPAD_BUTTON_LEFT)  c = (c + cols - 1) % cols;
        if (d & WPAD_BUTTON_RIGHT) c = (c + 1) % cols;
        if (d & WPAD_BUTTON_PLUS)  done = 1;
        if (d & WPAD_BUTTON_MINUS) done = 0;
        if (d & WPAD_BUTTON_B)     kb_back(buf, &len);
        int fire = -1;   /* tecla activada por A (puntero o foco) */

        /* ---- logica de teclas: puntero encima -> foco; A sobre una tecla ---- */
        for (i = 0; i < KB_ROWS; i++) for (k = 0; k < 10; k++) {
            int x = kx0 + k * (kw + gap), y = ky0 + i * (kh + gap);
            if (ui_hover(x, y, kw, kh)) { r = i; c = k; }
            if (ui_click(x, y, kw, kh)) {
                unsigned char ch = kb_rows[i][k];
                if (shift && ((ch >= 'a' && ch <= 'z') || ch == 0xF1)) ch -= 32;
                fire = 1000 + ch;
            }
        }
        for (k = 0; k < nsp; k++) {
            if (ui_hover(spx[k], sy, sw[k], kh)) { r = KB_ROWS; c = k; }
            if (ui_click(spx[k], sy, sw[k], kh)) fire = k;
        }
        /* si el puntero salio de una tecla, el foco queda donde estaba */

#ifdef KBD_BENCH
        unsigned kb_t1pre = KBT();
#endif
        /* ---- dibujo ---- */
        int did_present = 0;
        {
            kb_rect_t L[18]; int nl = 0;
            int phase = (int)((ticks_to_millisecs(gettime()) / 500) & 1);
            unsigned h = 2166136261u; for (const unsigned char *q = (const unsigned char *)buf; *q; q++) h = (h ^ *q) * 16777619u;
            int text_chg = h != kb_hash || phase != kb_phase;
            int ptr_chg = in_ptr.valid != pcv || (in_ptr.valid && (in_ptr.x != pcx || in_ptr.y != pcy));
            if (need_full) {
                /* capa estatica: fondo + capa oscura + panel + titulo + ayuda + marco del campo */
#ifdef KBD_BENCH
                unsigned kq0 = KBT();
#endif
                ui_backdrop(170);
#ifdef KBD_BENCH
                unsigned kq1 = KBT();
#endif
                ui_panel(px, py, pw, ph, title);
                if (hint) gfx_text_fit(&font_small, px + 22, py + 56, hint, C_DIM, pw - 44);
                gfx_rrect(fx, fy, fw, fh, 10, 0x0B0B0E, 255);
                gfx_rframe(fx, fy, fw, fh, 10, 2, C_ACCENT, 255);
                gfx_icon(IC_SEARCH, fx + 22, fy + fh / 2, 18, C_DIM);
                gfx_text_c(&font_small, SCR_W / 2, py + ph - 22, has_emo ? "A: tecla   B: borrar   +: aceptar   -: cancelar   2: emotes" : "A: tecla   B: borrar   +: buscar   -: cancelar   (tambien teclado USB)", C_FAINT);
                gfx_noclip();
#ifdef KBD_BENCH
                unsigned kq2 = KBT();
#endif
                if (kb_snap) { memcpy(kb_snap, canvas, SCR_W * SCR_H * sizeof(u32)); have_snap = 1; }
#ifdef KBD_BENCH
                kbl("full-layer: backdrop+dim %u us, panel+static %u us, snapshot memcpy %u us", kq1 - kq0, kq2 - kq1, KBT() - kq2);
#endif
                kb_rect_add(L, &nl, 0, 0, SCR_W, SCR_H);
                memset(kst, 0xFF, sizeof kst);
            } else if (have_snap) {
                if (ptr_chg) { if (pcv) KB_CURSOR_RECT(L, &nl, pcx, pcy); if (in_ptr.valid) KB_CURSOR_RECT(L, &nl, in_ptr.x, in_ptr.y); }
                if (text_chg) kb_rect_add(L, &nl, fx, fy, fx + fw, fy + fh);
                if (shift != pshift) kb_rect_add(L, &nl, kx0 - 3, ky0 - 3, kx0 + 10 * kw + 9 * gap + 3, sy + kh + 4);   /* Mayus: cambian todas las letras */
            }
            /* estado de cada tecla: bit0 foco, bit1 puntero encima, bit2 apretada */
            if (have_snap || !kb_snap) for (i = 0; i < NK; i++) {
                int x, y, w;
                if (i < KB_ROWS * 10) { x = kx0 + (i % 10) * (kw + gap); y = ky0 + (i / 10) * (kh + gap); w = kw; }
                else { int kk = i - KB_ROWS * 10; if (kk >= nsp) { kst[i] = 0; continue; } x = spx[kk]; y = sy; w = sw[kk]; }
                int foc = i < KB_ROWS * 10 ? (r == i / 10 && c == i % 10) : (r == KB_ROWS && c == i - KB_ROWS * 10);
                int hov = ui_hover(x, y, w, kh);
                unsigned char st = (foc ? 1 : 0) | (hov ? 2 : 0) | (hov && (in_ptr.held & WPAD_BUTTON_A) ? 4 : 0);
                if (st != kst[i]) { kst[i] = st; if (!need_full && shift == pshift) kb_rect_add(L, &nl, x - 3, y - 3, x + w + 3, y + kh + 4); }
            }
            if (nl && !need_full && !ptr_chg && in_ptr.valid) KB_CURSOR_RECT(L, &nl, in_ptr.x, in_ptr.y);   /* la flecha se redibuja sobre un rectangulo restaurado */
            if (nl) {
                kb_rects_merge(L, &nl);
                /* si algun rectangulo toca el campo, se repinta el campo completo (emote_preview_draw fija su propio recorte) */
                { int grew = 0; for (int q = 0; q < nl; q++) if (kb_isect(&L[q], fx, fy, fx + fw, fy + fh)) { if (fx < L[q].x0) L[q].x0 = fx; if (fy < L[q].y0) L[q].y0 = fy; if (fx + fw > L[q].x1) L[q].x1 = fx + fw; if (fy + fh > L[q].y1) L[q].y1 = fy + fh; grew = 1; } if (grew) kb_rects_merge(L, &nl); }
                for (int q = 0; q < nl; q++) {
                    const kb_rect_t *R = &L[q];
                    if (have_snap && !need_full) kb_restore(R);
                    gfx_clip(R->x0, R->y0, R->x1 - R->x0, R->y1 - R->y0);
                    /* contenido del campo */
                    if (kb_isect(R, fx, fy, fx + fw, fy + fh)) {
                        int cx = buf[0] && sui_kb_preview ? sui_kb_preview(buf, fx + 42, fy + 12, fw - 70) : -1;
                        if (cx < 0) {
                            const char *show = buf;
                            while (gfx_text_w(&font_title, show) > fw - 70 && *show) { const char *qq = show; utf8_next(&qq); show = qq; }
                            cx = fx + 44 + gfx_text(&font_title, fx + 42, fy + 9, show, C_TEXT);
                        }
                        gfx_clip(R->x0, R->y0, R->x1 - R->x0, R->y1 - R->y0);   /* la vista previa de emotes cambia el recorte */
                        if (!buf[0]) gfx_text(&font_body, fx + 44, fy + 14, placeholder, C_FAINT);
                        if (phase) gfx_rect(cx, fy + 10, 2, 25, C_ACCENT);
                    }
                    /* teclas que tocan el rectangulo */
                    for (i = 0; i < NK; i++) {
                        int x, y, w; const char *lab; char lb[4] = { 0 }; int ic = 0, st = BS_NORMAL, foc;
                        if (i < KB_ROWS * 10) {
                            int ri = i / 10, ci = i % 10;
                            x = kx0 + ci * (kw + gap); y = ky0 + ri * (kh + gap); w = kw;
                            if (!kb_isect(R, x - 3, y - 3, x + w + 3, y + kh + 4)) continue;
                            unsigned char ch = kb_rows[ri][ci];
                            if (shift && ((ch >= 'a' && ch <= 'z') || ch == 0xF1)) ch -= 32;
                            if (ch < 0x80) lb[0] = ch; else { lb[0] = 0xC0 | (ch >> 6); lb[1] = 0x80 | (ch & 0x3F); }
                            lab = lb; foc = (r == ri && c == ci);
                        } else {
                            int kk = i - KB_ROWS * 10;
                            if (kk >= nsp) continue;
                            x = spx[kk]; y = sy; w = sw[kk];
                            if (!kb_isect(R, x - 3, y - 3, x + w + 3, y + kh + 4)) continue;
                            st = kk == K_OK ? BS_ACCENT : (kk == K_SHIFT && shift ? BS_ACCENT : BS_NORMAL);
                            ic = kk == K_OK ? IC_SEARCH : (kk == K_BACK ? IC_BACK : 0);
                            lab = kk == K_OK ? accept : sl[kk]; foc = (r == KB_ROWS && c == kk);
                        }
                        { u32 sd = UI.down; UI.down = 0; ui_button(x, y, w, kh, lab, ic, st, foc); UI.down = sd; }
                    }
                }
                gfx_noclip();
                if (in_ptr.valid) ui_draw_cursor();
                if (need_full) gfx_ui_cache_rows(0, SCR_H);
                else { for (int q = 0; q < nl; q++) gfx_ui_mark_rect(L[q].x0, L[q].y0, L[q].x1, L[q].y1); gfx_ui_commit_rows(); }
#ifdef KBD_BENCH
                unsigned kb_t1 = KBT();
#endif
                gfx_composite(NULL, 0, 0, 0, 0, NULL);
                did_present = 1;
#ifdef KBD_BENCH
                { unsigned kb_t2 = KBT(); int ph2 = kb_frame < 90 ? 0 : (kb_frame < 180 ? 1 : 2);
                  if (!kb_first) { kb_first = kb_t2 - kb_t_open; kbl("first frame presented %u us after open (draw %u us, present %u us, convert %u us)", kb_first, kb_t1 - kb_t0, kb_t2 - kb_t1, gfx_last_conv_us); }
                  kb_sum_draw[ph2] += kb_t1 - kb_t0; kb_sum_pres[ph2] += kb_t2 - kb_t1; kb_sum_conv[ph2] += gfx_last_conv_us; kb_present_n[ph2]++;
                  if (kb_frame < 12 || kb_frame % 30 == 0) kbl("frame %u: draw %u us, present %u us (convert %u us)", kb_frame, kb_t1 - kb_t0, kb_t2 - kb_t1, gfx_last_conv_us); }
#endif
            } else {
                gfx_noclip();
                VIDEO_WaitVSync();   /* sin cambios: no se dibuja ni se presenta nada */
            }
            kb_hash = h; kb_phase = phase; pcv = in_ptr.valid; pcx = in_ptr.x; pcy = in_ptr.y; pshift = shift;
            need_full = !have_snap;   /* sin copia (MEM2 llena): se redibuja todo como antes */
        }
#ifdef KBD_BENCH
        {
            unsigned kb_t2 = KBT(); int ph = kb_frame < 90 ? 0 : (kb_frame < 180 ? 1 : 2);
            (void)kb_t1pre; (void)did_present;
            kb_n[ph]++; kb_loop[ph] += kb_t2 - kb_t_prev;
            if (kb_t2 - kb_t_prev > kb_max[ph]) kb_max[ph] = kb_t2 - kb_t_prev;
            kb_t_prev = kb_t2; kb_frame++;
            if (kb_frame == 241) {
                static const char *nm[3] = { "idle", "pointer-hover", "typing" };
                for (int q = 0; q < 3; q++) if (kb_n[q]) {
                    unsigned pn = kb_present_n[q] ? kb_present_n[q] : 1;
                    kbl("SUMMARY %-13s loops=%u presented=%u | per presented frame: draw=%u us present=%u us (convert %u us) cpu-work=%u us | avg loop=%u us max=%u us", nm[q], kb_n[q], kb_present_n[q], kb_sum_draw[q] / pn, kb_sum_pres[q] / pn, kb_sum_conv[q] / pn, (kb_sum_draw[q] + kb_sum_conv[q]) / pn, kb_loop[q] / kb_n[q], kb_max[q]);
                }
                kbl("DONE");
            }
        }
#endif

        if (fire < 0 && (d & WPAD_BUTTON_A) && !UI.consumed)
            fire = r < KB_ROWS ? -2 : c;
        if (fire == -2) {
            unsigned char ch = kb_rows[r][c];
            if (shift && ((ch >= 'a' && ch <= 'z') || ch == 0xF1)) ch -= 32;
            fire = 1000 + ch;
        }
        if (fire >= 1000) kb_char(buf, max, &len, fire - 1000);
        else switch (fire) {
            case K_SPACE:  kb_char(buf, max, &len, ' '); break;
            case K_BACK:   kb_back(buf, &len); break;
            case K_CLEAR:  len = 0; buf[0] = 0; break;
            case K_SHIFT:  shift = !shift; break;
            case K_CANCEL: done = 0; break;
            case K_OK:     done = 1; break;
            case K_EMOTE:  fire = -3; break;
        }
        if (has_emo && (d & WPAD_BUTTON_2)) fire = -3;
        if (fire == -3) {   /* rc14: grilla de emotes; inserta separado por espacios */
            char ins[96] = "";
            if (sui_kb_emote_hook(ins, sizeof ins) && ins[0]) {
                int need = strlen(ins) + 2;
                if (len + need <= max) {
                    if (len && buf[len - 1] != ' ') buf[len++] = ' ';
                    memcpy(buf + len, ins, strlen(ins)); len += strlen(ins);
                    if (buf[len - 1] != ' ') buf[len++] = ' ';
                    buf[len] = 0;
                } else ui_toast("No entra: el mensaje ya esta lleno.");
            }
            wait_release();
            need_full = 1;   /* la grilla de emotes pisó el lienzo: capa estatica de nuevo */
        }
    }
    gfx_ui_cache_invalidate();
    return done == 1;
}

int sui_text_input(const char *title,const char *hint,char *buf,int max){return sui_text_input_ex(title,hint,buf,max,"Buscar","Canal o slug");}

/* ---------------- lista (menu) ---------------- */
u32 sui_list(const char *title, const char *help, char **items, int n, int *sel) {
    int top = 0, i, rows = 8, rh = 38;
    int pw = 520, py = 36, ph = 60 + rows * rh + 34;
    int px = (SCR_W-pw)/2;
    wait_release();
    while (SYS_MainLoop()) {
        u32 d = in_poll();
        ui_begin(d);
        if (d & WPAD_BUTTON_HOME) { ui_quit = 1; return WPAD_BUTTON_HOME; }
        if (d & WPAD_BUTTON_UP)   { *sel = (*sel + n - 1) % n; }
        if (d & WPAD_BUTTON_DOWN) { *sel = (*sel + 1) % n; }
        if (*sel < top) top = *sel;
        if (*sel >= top + rows) top = *sel - rows + 1;
        int clicked = 0;
        ui_backdrop(160);
        ui_panel(px, py, pw, ph, title);
        for (i = 0; i < rows && top + i < n; i++) {
            int k = top + i, y = py + 54 + i * rh;
            char t[120], *nl;
            snprintf(t, sizeof(t), "%s", items[k]);
            nl = strchr(t, '\n');
            if (nl) *nl = 0;
            if (ui_hover(px + 10, y, pw - 20, rh - 4)) *sel = k;
            if (k == *sel) {
                gfx_rrect(px + 10, y, pw - 20, rh - 4, 8, C_PANEL2, 255);
                gfx_rrect(px + 10, y + 6, 4, rh - 16, 2, C_ACCENT, 255);
            }
            gfx_text_fit(&font_bold, px + 26, y + 3, t, k == *sel ? C_TEXT : C_DIM, pw - 50);
            if (nl && nl[1]) gfx_text_fit(&font_small, px + 26, y + 20, nl + 1, C_FAINT, pw - 50);
            if (ui_click(px + 10, y, pw - 20, rh - 4)) { *sel = k; clicked = 1; }
        }
        if (n > rows) {
            int bh = rows * rh, th = bh * rows / n, ty = py + 54 + (bh - th) * top / (n - rows);
            gfx_rrect(px + pw - 8, ty, 4, th, 2, C_LINE, 255);
        }
        int by = py + ph - 32;
        gfx_text(&font_small, px + 20, by + 8, help ? help : "A: elegir  B: volver", C_FAINT);
        int back = ui_button(px + pw - 104, by - 2, 88, 28, "Volver", IC_BACK, BS_NORMAL, 0);
        ui_present();
        if (clicked) return WPAD_BUTTON_A;
        if (back) return WPAD_BUTTON_B;
        if (UI.consumed) continue;
        if (d & WPAD_BUTTON_A) return WPAD_BUTTON_A;
        if (d & WPAD_BUTTON_B) return WPAD_BUTTON_B;
        if (d & WPAD_BUTTON_PLUS) return WPAD_BUTTON_PLUS;
        if (d & WPAD_BUTTON_MINUS) return WPAD_BUTTON_MINUS;
        if (d & WPAD_BUTTON_1) return WPAD_BUTTON_1;
        if (d & WPAD_BUTTON_2) return WPAD_BUTTON_2;
    }
    ui_quit = 1;
    return WPAD_BUTTON_HOME;
}

/* ---------------- visor de texto ---------------- */
int ui_wrap(const void *fontp, const char *text, int maxw, char ***out) {
    const Font *f=(const Font *)fontp;
    int cap=16,n=0;
    char **v=calloc(cap,sizeof(*v));
    *out=NULL;
    if(!v || maxw<=0) { free(v); return 0; }
    const char *p=text?text:"";
    for(;;) {
        const char *nl=strchr(p,'\n');size_t seg=nl?(size_t)(nl-p):strlen(p);
        char *line=malloc(seg+1);
        if(!line)goto oom;
        memcpy(line,p,seg);line[seg]=0;
        char *s=line;
        do {
            int take=strlen(s);
            while(take>0) {
                char save=s[take];s[take]=0;int w=gfx_text_w(f,s);s[take]=save;
                if(w<=maxw)break;
                int k=take-1;while(k>0&&s[k]!=' ')k--;
                take=k>0?k:take-1;
            }
            if(take<=0)take=*s?1:0;
            if(n>=cap) {
                int nc=cap*2;char **nv=realloc(v,nc*sizeof(*v));
                if(!nv){free(line);goto oom;}v=nv;cap=nc;
            }
            v[n]=malloc(take+1);
            if(!v[n]){free(line);goto oom;}
            memcpy(v[n],s,take);v[n][take]=0;n++;
            s+=take;while(*s==' ')s++;
        }while(*s);
        free(line);if(!nl)break;p=nl+1;
    }
    *out=v;return n;
oom:
    for(int i=0;i<n;i++)free(v[i]);free(v);return 0;
}

u32 sui_text_view(const char *title, const char *help, const char *text) {
    char **lines;
    int px = (SCR_W-540)/2, py = 30, pw = 540, ph = 420, lh = 19, vis = (ph - 100) / lh;
    int n = ui_wrap(&font_body, text ? text : "", pw - 56, &lines), scroll = 0, i;
    u32 ret = WPAD_BUTTON_B;
    wait_release();
    while (SYS_MainLoop()) {
        int maxs = n > vis ? n - vis : 0;
        u32 d = in_poll();
        ui_begin(d);
        if (d & WPAD_BUTTON_HOME) { ui_quit = 1; ret = WPAD_BUTTON_HOME; break; }
        if (d & WPAD_BUTTON_UP)    { if (scroll > 0) scroll--; }
        if (d & WPAD_BUTTON_DOWN)  { if (scroll < maxs) scroll++; }
        ui_backdrop(160);
        ui_panel(px, py, pw, ph, title);
        gfx_clip(px, py + 54, pw, vis * lh);
        for (i = 0; i < vis && scroll + i < n; i++) gfx_text(&font_body, px + 26, py + 56 + i * lh, lines[scroll + i], C_TEXT);
        gfx_noclip();
        int by = py + ph - 40;
        if (maxs) {
            if (ui_button(px + 20, by, 40, 30, NULL, IC_UP, BS_NORMAL, 0) && scroll > 0) scroll -= vis / 2;
            if (ui_button(px + 66, by, 40, 30, NULL, IC_DOWN, BS_NORMAL, 0)) scroll += vis / 2;
            if (scroll > maxs) scroll = maxs;
            if (scroll < 0) scroll = 0;
        }
        gfx_text(&font_small, px + 120, by + 8, help ? help : "A/B: volver", C_FAINT);
        int ok = ui_button(px + pw - 120, by, 100, 30, "OK", IC_OK, BS_ACCENT, !in_ptr.valid);
        ui_present();
        if (ok) { ret = WPAD_BUTTON_A; break; }
        if (UI.consumed) continue;
        if (d & WPAD_BUTTON_A)     { ret = WPAD_BUTTON_A; break; }
        if (d & WPAD_BUTTON_B)     { ret = WPAD_BUTTON_B; break; }
        if (d & WPAD_BUTTON_PLUS)  { ret = WPAD_BUTTON_PLUS; break; }
        if (d & WPAD_BUTTON_MINUS) { ret = WPAD_BUTTON_MINUS; break; }
    }
    for (i = 0; i < n; i++) free(lines[i]);
    free(lines);
    return ret;
}

void sui_message(const char *title, const char *text) { sui_text_view(title, "A/B: continuar", text); }

int sui_confirm(const char *title, const char *text) {
    char **lines;
    int px = (SCR_W-400)/2, pw = 400, i, n = ui_wrap(&font_body, text, pw - 48, &lines), ph = 120 + n * 19, py = (SCR_H - ph) / 2;
    /* Fail closed: No is default; allocation failure never opts in. */
    if (!lines || !n) return 0;
    int foc = 1, res = -1;
    wait_release();
    while (SYS_MainLoop() && res < 0) {
        u32 d = in_poll();
        ui_begin(d);
        if (d & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) foc ^= 1;
        ui_backdrop(160);
        ui_panel(px, py, pw, ph, title);
        for (i = 0; i < n; i++) gfx_text(&font_body, px + 24, py + 58 + i * 19, lines[i], C_TEXT);
        int by = py + ph - 46;
        if (ui_button(px + pw - 240, by, 100, 32, "Si", IC_OK, BS_ACCENT, foc == 0)) res = 1;
        if (ui_button(px + pw - 128, by, 100, 32, "No", IC_BACK, BS_NORMAL, foc == 1)) res = 0;
        ui_present();
        if (res >= 0 || UI.consumed) continue;
        if (d & WPAD_BUTTON_A) res = foc == 0;
        if (d & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) res = 0;
    }
    for (i = 0; i < n; i++) free(lines[i]);
    free(lines);
    UI.down = 0; UI.consumed = 1; /* no click-through into the parent menu */
    return res == 1;
}
