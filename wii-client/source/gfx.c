#include <stdio.h>
/*
 * gfx.c - lienzo virtual por software 640/854x480 (RGB) convertido a YUYV en el XFB.
 * Todo el arte (iconos, logo, cursor) se dibuja con primitivas propias.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <gccore.h>
#include "gfx.h"
#include "tv.h"
#include "mem2.h"
#include "guard.h"

static GXRModeObj mode_storage, *rmode;
static int aspect_mode=0, system_wide=0;
/* Full-width virtual square-pixel canvas, anamorphically encoded at scanout.
 * No centered 4:3 safe-area box in widescreen mode. */
int gfx_canvas_width=640;
int gfx_widescreen(void) { return aspect_mode?aspect_mode==2:system_wide; }
int gfx_aspect_mode(void) { return aspect_mode; }
/* rc12/rc6: zona segura para TVs con overscan (p. ej. Noblex). Todo el lienzo
 * (SCR_W x 480, 4:3 o 16:9) se escala al 90% dentro de un rectangulo con margen
 * de SAFE_X px a los lados y SAFE_Y px arriba/abajo del XFB de 640x480. Los
 * margenes repiten el borde del lienzo, asi los fondos siguen llegando al borde
 * y ningun texto/boton queda fuera de la pantalla. El video a pantalla completa
 * sigue ocupando toda la pantalla (sobre el video el TV recorta igual). */
/* rc15/rc10: el margen de los costados baja de 32 a 12 px en 4:3 y 16:9 (la UI llena el ancho del TV,
 * con un margen chico para el overscan). Los margenes ahora son negros:
 * antes repetian el borde del lienzo y en TVs que muestran todo se veian franjas de color. */
#define SAFE_X 12
#define SAFE_Y 24
#define BLACK_YUYV 0x10801080u
#define SAFE_W (FB_W - 2 * SAFE_X)
#define SAFE_H (SCR_H - 2 * SAFE_Y)
/* Mapas con filtro bilineal (pesos 0..255) para que el texto escalado no pierda trazos. */
static short colmap[FB_W], colmap1[FB_W], rowmap[SCR_H], rowmap1[SCR_H];
static unsigned char colw[FB_W], roww[SCR_H];
static int maps_w = -1;
static void safe_axis(int n_out, int lo, int n_safe, int n_src, short *m0, short *m1, unsigned char *w) {
    for (int i = 0; i < n_out; i++) {
        int c, f;
        if (i < lo) { c = 0; f = 0; }
        else if (i >= lo + n_safe) { c = n_src - 1; f = 0; }
        else {
            int num = (int)(((long long)(2 * (i - lo) + 1) * n_src * 256) / (2 * n_safe)) - 128;
            if (num < 0) num = 0;
            c = num >> 8; f = num & 255;
            if (c >= n_src - 1) { c = n_src - 1; f = 0; }
        }
        m0[i] = (short)c; m1[i] = (short)(c + 1 < n_src ? c + 1 : c); w[i] = (unsigned char)f;
    }
}
static void safe_maps(void) {
    if (maps_w == SCR_W) return;
    maps_w = SCR_W;
    safe_axis(FB_W, SAFE_X, SAFE_W, SCR_W, colmap, colmap1, colw);
    safe_axis(SCR_H, SAFE_Y, SAFE_H, SCR_H, rowmap, rowmap1, roww);
}
static inline u32 mix2(u32 a, u32 b, unsigned w) {
    if (!w || a == b) return a;
    unsigned iw = 256 - w;
    u32 rb = ((a & 0xff00ff) * iw + (b & 0xff00ff) * w) >> 8;
    u32 g = ((a & 0xff00) * iw + (b & 0xff00) * w) >> 8;
    return (rb & 0xff00ff) | (g & 0xff00);
}
static inline u32 rgb2yuyv(u32 a, u32 b);
/* una fila del XFB (fy, 0..479) en YUYV, desde el lienzo escalado a la zona segura */
/* color dominante de una fila del lienzo (para las franjas de arriba/abajo: sin rayas) */
static u32 __attribute__((unused)) edge_mode(int row) {
    u32 v[64]; int best = 0; u32 bc = 0;
    const u32 *s = canvas + row * SCR_W;
    for (int i = 0; i < 64; i++) v[i] = s[i * SCR_W / 64];
    for (int i = 0; i < 64; i++) { int c = 0; for (int j = i; j < 64; j++) c += v[j] == v[i]; if (c > best) { best = c; bc = v[i]; } if (best > 32) break; }
    return bc;
}
static void scan_row(int fy, u32 *d) {
    static u32 rowtmp[SCR_MAX_W];
    if (fy < SAFE_Y || fy >= SAFE_Y + SAFE_H) {   /* franjas de arriba/abajo: negro fijo */
        for (int x = 0; x < FB_W / 2; x++) d[x] = BLACK_YUYV;
        return;
    }
    const u32 *s = canvas + rowmap[fy] * SCR_W;
    unsigned wy = roww[fy];
    if (wy) {
        const u32 *s2 = canvas + rowmap1[fy] * SCR_W;
        for (int i = 0; i < SCR_W; i++) rowtmp[i] = mix2(s[i], s2[i], wy);
        s = rowtmp;
    }
    u32 la = 0xFFFFFFFF, lb = 0xFFFFFFFF, lv = 0;
    for (int x = 0; x < FB_W; x += 2) {
        u32 a = mix2(s[colmap[x]], s[colmap1[x]], colw[x]);
        u32 b = mix2(s[colmap[x + 1]], s[colmap1[x + 1]], colw[x + 1]);
        if (a != la || b != lb) { lv = rgb2yuyv(a, b); la = a; lb = b; }
        *d++ = lv;
    }
    d -= FB_W / 2;   /* costados: negro fijo */
    for (int x = 0; x < SAFE_X / 2; x++) d[x] = BLACK_YUYV;
    for (int x = (SAFE_X + SAFE_W) / 2; x < FB_W / 2; x++) d[x] = BLACK_YUYV;
}
/* filas del XFB afectadas por las filas [y0,y1) del lienzo */
static void safe_rows(int y0, int y1, int *f0, int *f1) {
    *f0 = y0 <= 0 ? 0 : SAFE_Y + y0 * SAFE_H / SCR_H - 1;
    *f1 = y1 >= SCR_H ? SCR_H : SAFE_Y + (y1 * SAFE_H + SCR_H - 1) / SCR_H + 1;
    if (*f0 < 0) *f0 = 0;
    if (*f1 > SCR_H) *f1 = SCR_H;
}
/* puntero IR (coordenadas del XFB 640x480) -> lienzo */
int gfx_pointer_x(int x) { return (x - SAFE_X) * SCR_W / SAFE_W; }
int gfx_pointer_y(int y) { return (y - SAFE_Y) * SCR_H / SAFE_H; }
static int ui_cache_valid;

static void *xfb[2];
static int cur;
/* Software canvas lives in MEM2 (allocated in gfx_init). Public API is the canvas pointer. */
#define CANVAS_BYTES ((u32)(SCR_MAX_W * SCR_H * sizeof(u32)))
static u32 *canvas_buf;
u32 *canvas;
static int cx0 = 0, cy0 = 0, cx1 = SCR_MAX_W, cy1 = SCR_H;
static int tY[3][256], tCb[3][256], tCr[3][256];
static unsigned ui_gen = 1, xfb_gen[2] = { 0, 0 }, xfb_sig[2] = { 0, 0 };

void gfx_set_aspect(int mode) {
    aspect_mode=(mode>=0&&mode<=2)?mode:0;
    gfx_canvas_width=gfx_widescreen()?854:640;
    gfx_noclip();if(canvas)memset(canvas,0,CANVAS_BYTES);
    ui_cache_valid=0;ui_gen++;xfb_gen[0]=xfb_gen[1]=0;
}

void gfx_init(void) {
    int i;
    twitchwii_mem2_init();
    if (!canvas_buf) {
        canvas_buf = twitchwii_mem2_alloc(CANVAS_BYTES, 32);
        canvas = canvas_buf;
    }
    VIDEO_Init();
    system_wide=CONF_GetAspectRatio()==CONF_ASPECT_16_9;
    gfx_set_aspect(0);
    mode_storage=*VIDEO_GetPreferredMode(NULL);rmode=&mode_storage;
    /* 704 active VI samples: 640 would squeeze circles by 640/704. */
    rmode->fbWidth=FB_W;
    rmode->viWidth=704;
    rmode->viXOrigin=(VI_MAX_WIDTH_NTSC-rmode->viWidth)/2;
    VIDEO_Configure(rmode);
    xfb[0] = SYS_AllocateFramebuffer(rmode);
    xfb[1] = SYS_AllocateFramebuffer(rmode);
    VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
    DCFlushRange(xfb[0], VIDEO_GetFrameBufferSize(rmode));
    DCFlushRange(xfb[1], VIDEO_GetFrameBufferSize(rmode));
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    for (i = 0; i < 256; i++) {
        tY[0][i] = 19595 * i; tY[1][i] = 38470 * i; tY[2][i] = 7471 * i;
        tCb[0][i] = -11059 * i; tCb[1][i] = -21709 * i; tCb[2][i] = 32768 * i;
        tCr[0][i] = 32768 * i; tCr[1][i] = -27439 * i; tCr[2][i] = -5329 * i;
    }
}

static inline u32 rgb2yuyv(u32 a, u32 b) {
    int r1 = (a >> 16) & 255, g1 = (a >> 8) & 255, b1 = a & 255;
    int r2 = (b >> 16) & 255, g2 = (b >> 8) & 255, b2 = b & 255;
    int y1 = (tY[0][r1] + tY[1][g1] + tY[2][b1] + 32768) >> 16;
    int y2 = (tY[0][r2] + tY[1][g2] + tY[2][b2] + 32768) >> 16;
    int cb = ((tCb[0][r1] + tCb[1][g1] + tCb[2][b1] + tCb[0][r2] + tCb[1][g2] + tCb[2][b2]) >> 17) + 128;
    int cr = ((tCr[0][r1] + tCr[1][g1] + tCr[2][b1] + tCr[0][r2] + tCr[1][g2] + tCr[2][b2]) >> 17) + 128;
    if (y1 > 255) y1 = 255;
    if (y2 > 255) y2 = 255;
    if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
    if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
    return ((u32)y1 << 24) | ((u32)cb << 16) | ((u32)y2 << 8) | (u32)cr;
}

void gfx_present(void) {
    guard_beat();
    cur ^= 1;
    xfb_gen[cur] = 0;
    u32 *dst = (u32 *)xfb[cur];
    int stride = rmode->fbWidth / 2, h = rmode->xfbHeight, x, y;
    int yoff = (h - SCR_H) / 2, xoff = (rmode->fbWidth - FB_W) / 4;
    if (yoff < 0) yoff = 0;
    if (xoff < 0) xoff = 0;
    safe_maps();
    for (y = 0; y < SCR_H && y + yoff < h; y++) {
        u32 *d = dst + (y + yoff) * stride + xoff;
        scan_row(y, d);
    }
    (void)x;
    DCFlushRange(xfb[cur], VIDEO_GetFrameBufferSize(rmode));
    VIDEO_SetNextFramebuffer(xfb[cur]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

void gfx_clip(int x, int y, int w, int h) {
    cx0 = x < 0 ? 0 : x; cy0 = y < 0 ? 0 : y;
    cx1 = x + w > SCR_W ? SCR_W : x + w; cy1 = y + h > SCR_H ? SCR_H : y + h;
}
void gfx_noclip(void) { cx0 = 0; cy0 = 0; cx1 = SCR_W; cy1 = SCR_H; }

static inline u32 blend(u32 d, u32 c, int a) {
    a += a >> 7;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int r = dr + (((((int)(c >> 16) & 255) - dr) * a) >> 8);
    int g = dg + (((((int)(c >> 8) & 255) - dg) * a) >> 8);
    int b = db + (((((int)c & 255) - db) * a) >> 8);
    return (r << 16) | (g << 8) | b;
}
u32 gfx_mix(u32 a, u32 b, int t) { return blend(a, b, t); }

static inline void px(int x, int y, u32 c, int a) {
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || a <= 0) return;
    u32 *p = &canvas[y * SCR_W + x];
    *p = a >= 255 ? c : blend(*p, c, a);
}

void gfx_clear(u32 c) { int i; for (i = 0; i < SCR_W * SCR_H; i++) canvas[i] = c; }

void gfx_rect_a(int x, int y, int w, int h, u32 c, int a) {
    int i, j;
    if (x < cx0) { w -= cx0 - x; x = cx0; }
    if (y < cy0) { h -= cy0 - y; y = cy0; }
    if (x + w > cx1) w = cx1 - x;
    if (y + h > cy1) h = cy1 - y;
    if (w <= 0 || h <= 0 || a <= 0) return;
    for (j = 0; j < h; j++) {
        u32 *p = canvas + (y + j) * SCR_W + x;
        if (a >= 255) for (i = 0; i < w; i++) p[i] = c;
        else for (i = 0; i < w; i++) p[i] = blend(p[i], c, a);
    }
}
void gfx_rect(int x, int y, int w, int h, u32 c) { gfx_rect_a(x, y, w, h, c, 255); }

void gfx_vgrad(int x, int y, int w, int h, u32 top, u32 bot) {
    int j;
    for (j = 0; j < h; j++) gfx_rect(x, y + j, w, 1, blend(top, bot, h > 1 ? j * 255 / (h - 1) : 0));
}

/* cobertura AA aproximada de un pixel respecto de un circulo de radio r (en 1/16 px) */
static int cover(int dx16, int dy16, int r16) {
    int d2 = dx16 * dx16 + dy16 * dy16;
    int ri = r16 - 8, ro = r16 + 8;
    if (d2 <= ri * ri) return 255;
    if (d2 >= ro * ro) return 0;
    int d = (int)sqrtf((float)d2);
    return (ro - d) * 255 / 16;
}

void gfx_rrect(int x, int y, int w, int h, int r, u32 c, int a) {
    int i, j;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    if (r <= 0) { gfx_rect_a(x, y, w, h, c, a); return; }
    gfx_rect_a(x, y + r, w, h - 2 * r, c, a);
    gfx_rect_a(x + r, y, w - 2 * r, r, c, a);
    gfx_rect_a(x + r, y + h - r, w - 2 * r, r, c, a);
    for (j = 0; j < r; j++) for (i = 0; i < r; i++) {
        int dx = (r - i) * 16 - 8, dy = (r - j) * 16 - 8;
        int cv = cover(dx, dy, r * 16) * a / 255;
        if (!cv) continue;
        px(x + i, y + j, c, cv);
        px(x + w - 1 - i, y + j, c, cv);
        px(x + i, y + h - 1 - j, c, cv);
        px(x + w - 1 - i, y + h - 1 - j, c, cv);
    }
}

void gfx_rframe(int x, int y, int w, int h, int r, int t, u32 c, int a) {
    /* marco: bordes rectos + esquinas en anillo */
    int i, j;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    gfx_rect_a(x + r, y, w - 2 * r, t, c, a);
    gfx_rect_a(x + r, y + h - t, w - 2 * r, t, c, a);
    gfx_rect_a(x, y + r, t, h - 2 * r, c, a);
    gfx_rect_a(x + w - t, y + r, t, h - 2 * r, c, a);
    for (j = 0; j < r; j++) for (i = 0; i < r; i++) {
        int dx = (r - i) * 16 - 8, dy = (r - j) * 16 - 8;
        int cv = cover(dx, dy, r * 16) - cover(dx, dy, (r - t) * 16);
        cv = cv * a / 255;
        if (cv <= 0) continue;
        px(x + i, y + j, c, cv);
        px(x + w - 1 - i, y + j, c, cv);
        px(x + i, y + h - 1 - j, c, cv);
        px(x + w - 1 - i, y + h - 1 - j, c, cv);
    }
}

void gfx_circle(int cx, int cy, int r, u32 c, int a) {
    int i, j;
    for (j = -r - 1; j <= r; j++) for (i = -r - 1; i <= r; i++) {
        int cv = cover(i * 16 + 8, j * 16 + 8, r * 16);
        if (cv) px(cx + i, cy + j, c, cv * a / 255);
    }
}

void gfx_ring(int cx, int cy, int r, int t, u32 c, int a) {
    int i, j;
    for (j = -r - 1; j <= r; j++) for (i = -r - 1; i <= r; i++) {
        int cv = cover(i * 16 + 8, j * 16 + 8, r * 16) - cover(i * 16 + 8, j * 16 + 8, (r - t) * 16);
        if (cv > 0) px(cx + i, cy + j, c, cv * a / 255);
    }
}

void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2, u32 c) {
    /* relleno por semiplanos con 4x4 submuestras en los bordes */
    int minx = x0, maxx = x0, miny = y0, maxy = y0, x, y;
    if (x1 < minx) minx = x1; if (x2 < minx) minx = x2;
    if (x1 > maxx) maxx = x1; if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1; if (y2 < miny) miny = y2;
    if (y1 > maxy) maxy = y1; if (y2 > maxy) maxy = y2;
    int area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (!area) return;
    int sgn = area > 0 ? 1 : -1;
    for (y = miny; y <= maxy; y++) for (x = minx; x <= maxx; x++) {
        int n = 0, sx, sy;
        for (sy = 0; sy < 4; sy++) for (sx = 0; sx < 4; sx++) {
            float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
            float e0 = ((x1 - x0) * (fy - y0) - (y1 - y0) * (fx - x0)) * sgn;
            float e1 = ((x2 - x1) * (fy - y1) - (y2 - y1) * (fx - x1)) * sgn;
            float e2 = ((x0 - x2) * (fy - y2) - (y0 - y2) * (fx - x2)) * sgn;
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) n++;
        }
        if (n) px(x, y, c, n * 255 / 16);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, int th, u32 c) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy), i;
    if (!steps) steps = 1;
    for (i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps, y = y0 + dy * i / steps;
        if (th <= 1) px(x, y, c, 255); else gfx_circle(x, y, th / 2, c, 255);
    }
}

void gfx_blit(const u32 *p, int pw, int ph, int x, int y) {
    int j, i;
    for (j = 0; j < ph; j++) {
        int yy = y + j;
        if (yy < cy0 || yy >= cy1) continue;
        for (i = 0; i < pw; i++) {
            int xx = x + i;
            if (xx < cx0 || xx >= cx1) continue;
            canvas[yy * SCR_W + xx] = p[j * pw + i];
        }
    }
}

int utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    int c = *s;
    if (!c) return 0;
    if (c < 0x80) { *ps += 1; return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) { *ps += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) { *ps += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { *ps += 4; return '?'; }
    *ps += 1; return '?';
}

static const Glyph *glyph(const Font *f, int cp) {
    if (cp < 32 || cp > 255 || (cp >= 127 && cp < 160)) cp = '?';
    return &f->g[cp - 32];
}

static void draw_glyph(const Font *f, const Glyph *g, int pen, int y, u32 c) {
    const unsigned char *b = f->bits + g->off;
    int gx = pen + g->xoff, gy = y + g->yoff, i, j;
    for (j = 0; j < g->h; j++) {
        int yy = gy + j;
        if (yy < cy0 || yy >= cy1) continue;
        for (i = 0; i < g->w; i++) {
            int xx = gx + i, a = b[j * g->w + i];
            if (!a || xx < cx0 || xx >= cx1) continue;
            u32 *p = &canvas[yy * SCR_W + xx];
            *p = a == 255 ? c : blend(*p, c, a);
        }
    }
}

int gfx_text(const Font *f, int x, int y, const char *s, u32 c) {
    int pen = x, cp;
    while ((cp = utf8_next(&s))) { const Glyph *g = glyph(f, cp); draw_glyph(f, g, pen, y, c); pen += g->adv; }
    return pen - x;
}

int gfx_text_w(const Font *f, const char *s) {
    int w = 0, cp;
    while ((cp = utf8_next(&s))) w += glyph(f, cp)->adv;
    return w;
}

int gfx_text_fit(const Font *f, int x, int y, const char *s, u32 c, int maxw) {
    if (gfx_text_w(f, s) <= maxw) return gfx_text(f, x, y, s, c);
    int dots = glyph(f, '.')->adv * 3, pen = x, cp;
    while ((cp = utf8_next(&s))) {
        const Glyph *g = glyph(f, cp);
        if (pen + g->adv + dots > x + maxw) break;
        draw_glyph(f, g, pen, y, c); pen += g->adv;
    }
    pen += gfx_text(f, pen, y, "...", c);
    return pen - x;
}

void gfx_text_c(const Font *f, int cx, int y, const char *s, u32 c) { gfx_text(f, cx - gfx_text_w(f, s) / 2, y, s, c); }

/* ---------------- iconos (arte original con primitivas) ---------------- */
void gfx_icon(int ic, int cx, int cy, int s, u32 c) {
    int h = s / 2, t = s / 8 > 1 ? s / 8 : 2;
    switch (ic) {
    case IC_HEART:
        gfx_circle(cx-s/4,cy-s/6,s/4+1,c,255);gfx_circle(cx+s/4,cy-s/6,s/4+1,c,255);
        gfx_tri(cx-s/2,cy,cx+s/2,cy,cx,cy+s/2,c);break;
    case IC_NOTE: /* corchea: cabeza + plica + bandera */
        gfx_circle(cx - s / 5, cy + s / 4, s / 5 + 1, c, 255);
        gfx_rect(cx - s / 5 + s / 5 - 1, cy - h, t, h + s / 4, c);
        gfx_line(cx + 1, cy - h + 1, cx + s / 3, cy - s / 6, t, c);
        break;
    case IC_SEARCH:
        gfx_ring(cx - s / 8, cy - s / 8, s / 3, t, c, 255);
        gfx_line(cx + s / 8, cy + s / 8, cx + h - 1, cy + h - 1, t + 1, c);
        break;
    case IC_SD: /* tarjeta SD */
        gfx_rrect(cx - s / 3, cy - h, s * 2 / 3, s, 2, c, 255);
        gfx_tri(cx + s / 3, cy - h, cx + s / 3, cy - h + s / 4, cx + s / 3 - s / 4, cy - h, 0x202020);
        gfx_rect(cx - s / 5, cy - h + 3, 2, s / 4, 0x202020);
        gfx_rect(cx - s / 5 + 4, cy - h + 3, 2, s / 4, 0x202020);
        gfx_rect(cx - s / 5 + 8, cy - h + 3, 2, s / 4, 0x202020);
        break;
    case IC_MENU:
        gfx_rrect(cx - h, cy - h + 2, s, t, 1, c, 255);
        gfx_rrect(cx - h, cy - t / 2, s, t, 1, c, 255);
        gfx_rrect(cx - h, cy + h - 2 - t, s, t, 1, c, 255);
        break;
    case IC_PLAY: gfx_tri(cx - s / 3, cy - h, cx - s / 3, cy + h, cx + h, cy, c); break;
    case IC_PAUSE:
        gfx_rrect(cx - s / 3, cy - h, s / 4 + 1, s, 1, c, 255);
        gfx_rrect(cx + s / 3 - s / 4, cy - h, s / 4 + 1, s, 1, c, 255);
        break;
    case IC_NEXT:
        gfx_tri(cx - h, cy - h, cx - h, cy + h, cx + s / 4, cy, c);
        gfx_rect(cx + s / 4, cy - h, t + 1, s, c);
        break;
    case IC_PREV:
        gfx_tri(cx + h, cy - h, cx + h, cy + h, cx - s / 4, cy, c);
        gfx_rect(cx - s / 4 - t - 1, cy - h, t + 1, s, c);
        break;
    case IC_STOP: gfx_rrect(cx - s * 2 / 5, cy - s * 2 / 5, s * 4 / 5, s * 4 / 5, 2, c, 255); break;
    case IC_VOLUP: case IC_VOLDN:
        gfx_rect(cx - h, cy - s / 6, s / 4, s / 3, c);
        gfx_tri(cx - h + s / 5, cy - s / 6, cx - h + s / 5, cy + s / 6, cx + s / 8, cy - h + 1, c);
        gfx_tri(cx - h + s / 5, cy + s / 6, cx + s / 8, cy + h - 1, cx + s / 8, cy - h + 1, c);
        gfx_rect(cx + s / 4, cy - 1, s / 4 + 1, t, c);
        if (ic == IC_VOLUP) gfx_rect(cx + s / 4 + s / 8 - t / 2 + 1, cy - s / 8 - 1, t, s / 4 + t, c);
        break;
    case IC_USER:
        gfx_circle(cx,cy-s/5,s/5,c,255);
        gfx_rrect(cx-s/3,cy+s/12,s*2/3,s/3,s/7,c,255);break;
    case IC_LOCK:
        gfx_rrect(cx-s/3,cy-s/12,s*2/3,s/2,s/12,c,255);
        gfx_line(cx-s/5,cy-s/12,cx-s/5,cy-s/3,t+1,c);
        gfx_line(cx+s/5,cy-s/12,cx+s/5,cy-s/3,t+1,c);
        gfx_line(cx-s/5,cy-s/3,cx+s/5,cy-s/3,t+1,c);break;
    case IC_CHAT:
        gfx_rrect(cx-h,cy-h,s,s*3/4,s/8,c,255);
        gfx_tri(cx-s/3,cy+s/4,cx-s/3,cy+h,cx+s/8,cy+s/4,c);break;
    case IC_UP: gfx_tri(cx - h, cy + s / 4, cx + h, cy + s / 4, cx, cy - s / 4, c); break;
    case IC_DOWN: gfx_tri(cx - h, cy - s / 4, cx + h, cy - s / 4, cx, cy + s / 4, c); break;
    case IC_BACK: gfx_line(cx + s / 4, cy - h + 2, cx - s / 4, cy, t + 1, c); gfx_line(cx - s / 4, cy, cx + s / 4, cy + h - 2, t + 1, c); break;
    case IC_OK: gfx_line(cx - h + 2, cy, cx - s / 8, cy + s / 3, t + 1, c); gfx_line(cx - s / 8, cy + s / 3, cx + h - 1, cy - s / 3, t + 1, c); break;
    case IC_SAVE: /* flecha hacia abajo sobre una bandeja */
        gfx_rect(cx - t / 2, cy - h, t + 1, s * 2 / 3, c);
        gfx_tri(cx - s / 3, cy, cx + s / 3, cy, cx, cy + s / 4, c);
        gfx_rect(cx - h, cy + h - t, s, t, c);
        break;
    case IC_EQ: { /* barras de ecualizador (indicador "sonando") */
        int bw = s / 5;
        gfx_rect(cx - h, cy + h - s * 3 / 5, bw, s * 3 / 5, c);
        gfx_rect(cx - bw / 2, cy - h, bw, s, c);
        gfx_rect(cx + h - bw, cy + h - s * 2 / 5, bw, s * 2 / 5, c);
        break; }
    }
}

/* ---------------- video YUV 4:2:0 -> pantalla ---------------- */
static int vt_init = 0;
static int tYv[256], tRv[256], tGu[256], tGv[256], tBu[256];
static void vtab(void) {
    int i;
    if (vt_init) return;
    for (i = 0; i < 256; i++) {
        tYv[i] = (int)(1.164f * (i - 16) * 65536);
        tRv[i] = (int)(1.596f * (i - 128) * 65536);
        tGu[i] = (int)(0.391f * (i - 128) * 65536);
        tGv[i] = (int)(0.813f * (i - 128) * 65536);
        tBu[i] = (int)(2.018f * (i - 128) * 65536);
    }
    vt_init = 1;
}
static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }



void gfx_video_to_canvas(const tv_pic_t *p, int x, int y, int w, int h, int y0, int y1) {
    int j, i;
    static int xm[SCR_MAX_W];
    if (!p || w <= 0 || h <= 0) return;
    vtab();
    for (i = 0; i < w && i < SCR_W; i++) xm[i] = i * p->w / w;
    if (y0 < y) y0 = y;
    if (y1 > y + h) y1 = y + h;
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    for (j = y0; j < y1; j++) {
        int sy = (j - y) * p->h / h;
        const u8 *Y = p->y + sy * p->w, *U = p->u + (sy >> 1) * (p->w >> 1), *V = p->v + (sy >> 1) * (p->w >> 1);
        u32 *d = canvas + j * SCR_W;
        for (i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < cx0 || xx >= cx1) continue;
            int sx = xm[i], yy = tYv[Y[sx]], u = U[sx >> 1], v = V[sx >> 1];
            int r = (yy + tRv[v]) >> 16, g = (yy - tGu[u] - tGv[v]) >> 16, b = (yy + tBu[u]) >> 16;
            d[xx] = (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b);
        }
    }
}

/* ---- composicion barata: UI cacheada en YUYV + video YUV directo al XFB ---- */
static u32 *uicache = NULL;     /* lienzo ya convertido a YUYV (640x480) */
int gfx_present_count = 0;

void gfx_ui_cache_rows(int y0, int y1) {
    int x, y;
    if (!uicache) uicache = memalign(32, FB_W * SCR_H * 2);
    if (y0 < 0) y0 = 0;
    if (y1 > SCR_H) y1 = SCR_H;
    if (!uicache) return;
    ui_gen++;ui_cache_valid=1;
    safe_maps();
    { int f0, f1; safe_rows(y0, y1, &f0, &f1); y0 = f0; y1 = f1; }
    for (y = y0; y < y1; y++) {
        u32 *d = uicache + y * (FB_W / 2);
        scan_row(y, d);
    }
    (void)x;
}

/* rowsrc[y]: 0 = negro (o video), 1 = fila de la UI cacheada (video encima), 2 = UI opaca (sin video) */
/* rc14: actualizar solo pedazos de la UI (emotes animados del chat) sin invalidar todo
 * el cache: se marcan rectangulos del lienzo, se reconvierten solo esas columnas de esas
 * filas a YUYV y gfx_composite las copia a cada XFB aunque este en modo parcial. */
static short ui_mx0[SCR_H], ui_mx1[SCR_H], pend_c0[2][SCR_H], pend_c1[2][SCR_H];
static int ui_mark_any, pend_any[2];
void gfx_ui_mark_rect(int x0, int y0, int x1, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x0 < 0) x0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (x1 <= x0) return;
    for (int y = y0; y < y1; y++) {
        if (ui_mx1[y] <= ui_mx0[y]) { ui_mx0[y] = x0; ui_mx1[y] = x1; }
        else { if (x0 < ui_mx0[y]) ui_mx0[y] = x0; if (x1 > ui_mx1[y]) ui_mx1[y] = x1; }
        ui_mark_any = 1;
    }
}
static void scan_row_part(int fy, u32 *d, int c0, int c1) {
    if (fy < SAFE_Y || fy >= SAFE_Y + SAFE_H) return;
    const u32 *s = canvas + rowmap[fy] * SCR_W, *s2 = canvas + rowmap1[fy] * SCR_W;
    unsigned wy = roww[fy];
    if (c0 < SAFE_X) c0 = SAFE_X;
    if (c1 > SAFE_X + SAFE_W) c1 = SAFE_X + SAFE_W;
    for (int x = c0; x < c1; x += 2) {
        u32 a0 = s[colmap[x]], a1 = s[colmap1[x]], b0 = s[colmap[x + 1]], b1 = s[colmap1[x + 1]];
        if (wy) { a0 = mix2(a0, s2[colmap[x]], wy); a1 = mix2(a1, s2[colmap1[x]], wy); b0 = mix2(b0, s2[colmap[x + 1]], wy); b1 = mix2(b1, s2[colmap1[x + 1]], wy); }
        d[x / 2] = rgb2yuyv(mix2(a0, a1, colw[x]), mix2(b0, b1, colw[x + 1]));
    }
}
int gfx_ui_commit_rows(void) {
    static short f0s[SCR_H], f1s[SCR_H];
    int n = 0;
    if (!ui_mark_any) return 0;
    ui_mark_any = 0;
    if (!uicache || !ui_cache_valid) { memset(ui_mx0, 0, sizeof ui_mx0); memset(ui_mx1, 0, sizeof ui_mx1); return 0; }
    safe_maps();
    for (int f = 0; f < SCR_H; f++) { f0s[f] = FB_W; f1s[f] = 0; }
    for (int y = 0; y < SCR_H; y++) if (ui_mx1[y] > ui_mx0[y]) {
        int r0, r1, c0 = FB_W, c1 = 0;
        safe_rows(y, y + 1, &r0, &r1);
        for (int i = 0; i < FB_W; i++) if (colmap1[i] >= ui_mx0[y] && colmap[i] < ui_mx1[y]) { if (i < c0) c0 = i; c1 = i + 1; }
        c0 &= ~1; c1 = (c1 + 1) & ~1; if (c1 > FB_W) c1 = FB_W;
        for (int f = r0; f < r1; f++) { if (c0 < f0s[f]) f0s[f] = c0; if (c1 > f1s[f]) f1s[f] = c1; }
        ui_mx0[y] = ui_mx1[y] = 0;
    }
    for (int f = 0; f < SCR_H; f++) if (f1s[f] > f0s[f]) {
        scan_row_part(f, uicache + f * (FB_W / 2), f0s[f], f1s[f]);
        for (int k = 0; k < 2; k++) {
            if (pend_c1[k][f] <= pend_c0[k][f]) { pend_c0[k][f] = f0s[f]; pend_c1[k][f] = f1s[f]; }
            else { if (f0s[f] < pend_c0[k][f]) pend_c0[k][f] = f0s[f]; if (f1s[f] > pend_c1[k][f]) pend_c1[k][f] = f1s[f]; }
        }
        n += f1s[f] - f0s[f];
    }
    if (n) pend_any[0] = pend_any[1] = 1;
    return n;   /* pixeles del XFB reconvertidos */
}
void gfx_get_clip(int *x0, int *y0, int *x1, int *y1) { *x0 = cx0; *y0 = cy0; *x1 = cx1; *y1 = cy1; }
void gfx_composite(const tv_pic_t *p, int vx, int vy, int vw, int vh, const unsigned char *rowsrc) {
    static int xm[FB_W];
    if (!uicache || !ui_cache_valid) gfx_ui_cache_rows(0, SCR_H);
    if (!uicache) { gfx_present(); return; }
    int full=vw==SCR_W, wide=gfx_widescreen();
    /* UI coordinates are genuine 854x480 / 640x480 square-pixel coordinates.
       Only the final scanout is compressed into the fixed 640-pixel XFB. */
    safe_maps();
    /* rc12: el rectangulo del video pasa por la misma zona segura que la UI (salvo pantalla completa). */
    int bx=full?0:SAFE_X+vx*SAFE_W/SCR_W;
    int by=full?0:SAFE_Y+vy*SAFE_H/SCR_H,bw=full?FB_W:vw*SAFE_W/SCR_W,bh=full?SCR_H:vh*SAFE_H/SCR_H;
    int rw=bw,rh=bh;
    if(p&&p->w>0&&p->h>0&&bw>0&&bh>0) {
        float dar=p->aspect>0?p->aspect:(float)p->w/p->h;
        float ratio=dar/(wide?4.0f/3.0f:1.0f);
        if((float)bw/bh>ratio)rw=(int)(bh*ratio);else rh=(int)(bw/ratio);
    }
    rw &= ~1;
    int rx=(bx+(bw-rw)/2)&~1,ry=by+(bh-rh)/2;
    if(p&&rw>0)for(int x=0;x<rw&&x<FB_W;x++)xm[x]=x*p->w/rw;
    cur^=1;u32 *dst=(u32 *)xfb[cur];
    int stride=rmode->fbWidth/2,h=rmode->xfbHeight,yoff=(h-SCR_H)/2,xoff=(rmode->fbWidth-FB_W)/4;
    if(yoff<0)yoff=0;
    if(xoff<0)xoff=0;
    unsigned sig=2166136261u^(rx*7+ry*131+rw*8191+rh*524287)^(p?0x5bd1e995:0);
    if(rowsrc)for(int y=0;y<SCR_H;y++)sig=(sig^rowsrc[y])*16777619u;
    int partial=xfb_gen[cur]==ui_gen&&xfb_sig[cur]==sig&&p;
    xfb_gen[cur]=ui_gen;xfb_sig[cur]=sig;
    if(partial&&pend_any[cur])for(int y=0;y<SCR_H&&y+yoff<h;y++)if(pend_c1[cur][y]>pend_c0[cur][y]){
        u32 *d=dst+(y+yoff)*stride+xoff;
        int src=rowsrc?rowsrc[rowmap[y]]:1,c0=pend_c0[cur][y]/2,c1=pend_c1[cur][y]/2;
        if(src)memcpy(d+c0,uicache+y*(FB_W/2)+c0,(c1-c0)*4);
        else for(int x=c0;x<c1;x++)d[x]=0x10801080;
        if(p&&src!=2&&y>=by&&y<by+bh)for(int x=bx/2>c0?bx/2:c0;x<(bx+bw)/2&&x<c1;x++)if(x>=0)d[x]=0x10801080;
    }
    if(pend_any[cur]){memset(pend_c0[cur],0,sizeof pend_c0[cur]);memset(pend_c1[cur],0,sizeof pend_c1[cur]);pend_any[cur]=0;}
    int y0=partial?ry:0,y1=partial?ry+rh:SCR_H;
    if(y0<0)y0=0;
    if(y1>SCR_H)y1=SCR_H;
    for(int y=y0;y<y1&&y+yoff<h;y++) {
        u32 *d=dst+(y+yoff)*stride+xoff;
        int src=rowsrc?rowsrc[rowmap[y]]:1;
        if(!partial) {
            if(src)memcpy(d,uicache+y*(FB_W/2),FB_W*2);
            else for(int x=0;x<FB_W/2;x++)d[x]=0x10801080;
            if(p&&src!=2&&y>=by&&y<by+bh)for(int x=bx/2;x<(bx+bw)/2&&x<FB_W/2;x++)if(x>=0)d[x]=0x10801080;
        }
        if(!p||src==2||rw<=0||rh<=0||y<ry||y>=ry+rh)continue;
        int sy=(y-ry)*p->h/rh;
        const u8 *Y=p->y+sy*p->w,*U=p->u+(sy>>1)*(p->w>>1),*V=p->v+(sy>>1)*(p->w>>1);
        for(int x=0;x<rw&&rx+x+1<FB_W;x+=2) {
            if(rx+x<0)continue;
            int s0=xm[x],s1=xm[x+1];
            d[(rx+x)/2]=((u32)Y[s0]<<24)|((u32)U[s0>>1]<<16)|((u32)Y[s1]<<8)|V[s0>>1];
        }
    }
    DCFlushRange(xfb[cur],VIDEO_GetFrameBufferSize(rmode));
    VIDEO_SetNextFramebuffer(xfb[cur]);VIDEO_Flush();VIDEO_WaitVSync();gfx_present_count++;
}

/* ---- TwitchWii: imagenes con esquinas redondeadas / circulo (avatares) ---- */
void gfx_blit_round(const u32 *p, int pw, int ph, int x, int y, int r) {
    int j, i;
    if (!p) return;
    for (j = 0; j < ph; j++) {
        int yy = y + j;
        if (yy < cy0 || yy >= cy1) continue;
        for (i = 0; i < pw; i++) {
            int xx = x + i;
            if (xx < cx0 || xx >= cx1) continue;
            if (r > 0) {
                int dx = i < r ? r - i : (i >= pw - r ? i - (pw - r - 1) : 0);
                int dy = j < r ? r - j : (j >= ph - r ? j - (ph - r - 1) : 0);
                if (dx && dy) {
                    int d2 = dx * dx + dy * dy, r2 = r * r;
                    if (d2 > r2 + r) continue;
                    if (d2 > r2 - r) { canvas[yy * SCR_W + xx] = gfx_mix(canvas[yy * SCR_W + xx], p[j * pw + i], 128); continue; }
                }
            }
            canvas[yy * SCR_W + xx] = p[j * pw + i];
        }
    }
}

/* escala (vecino mas cercano) una imagen al rectangulo dado */
void gfx_blit_scaled(const u32 *p, int pw, int ph, int x, int y, int w, int h) {
    int j, i;
    if (!p || w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < cy0 || yy >= cy1) continue;
        const u32 *row = p + (j * ph / h) * pw;
        for (i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < cx0 || xx >= cx1) continue;
            canvas[yy * SCR_W + xx] = row[i * pw / w];
        }
    }
}

/* Resample and mask in one pass, without allocating per-frame avatar buffers. */
void gfx_blit_scaled_round(const u32 *p, int pw, int ph, int x, int y, int w, int h, int r) {
    if (!p || pw <= 0 || ph <= 0 || w <= 0 || h <= 0) return;
    int i,j; for (j=0;j<h;j++) {
        int yy=y+j; if(yy<cy0||yy>=cy1) continue;
        for(i=0;i<w;i++) {
            int xx=x+i; if(xx<cx0||xx>=cx1) continue;
            int dx=i<r?r-i:(i>=w-r?i-(w-r-1):0);
            int dy=j<r?r-j:(j>=h-r?j-(h-r-1):0);
            if(dx && dy && dx*dx+dy*dy>r*r+r) continue;
            canvas[yy*SCR_W+xx]=p[(j*ph/h)*pw+i*pw/w];
        }
    }
}
