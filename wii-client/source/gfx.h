/*
 * gfx.h - capa grafica por software: lienzo RGB 640/854x480 -> XFB YUYV (doble buffer)
 * Primitivas propias (rectangulos redondeados, circulos, triangulos, iconos)
 * y texto antialias con DejaVu Sans rasterizada (tools/gen_font.py).
 */
#ifndef GFX_H
#define GFX_H
#include <gctypes.h>

#define FB_W 640
#define SCR_MAX_W 854
extern int gfx_canvas_width;
#define SCR_W (gfx_canvas_width)
#define SCR_H 480

typedef struct { short w, h, xoff, yoff, adv; int off; } Glyph;
typedef struct { int line_h; int ascent; const Glyph *g; const unsigned char *bits; } Font;
extern const Font font_title, font_bold, font_body, font_small, font_big;
/* CJK glyph subset (cjk_data.c): 4-bit alpha, sorted code points. Sets: names (always) + 1 ja, 2 zh-Hans, 3 zh-Hant, 4 ko (loaded on demand).
 * Sizes: 0 = small (14px), 1 = body (15px), 2 = bold/title (16px), 3 = big (32px, only the "Browse" heading). */
typedef struct { int n; const unsigned short *cp; const Glyph *g; const unsigned char *bits; } CjkSet;
extern const CjkSet cjk_names[4];                    /* language names: always available */
extern const unsigned char *const cjk_zdata[4];       /* zlib blobs of the ja/zh/zt/ko sets, inflated only while active */
extern const unsigned cjk_zlen[4], cjk_rawlen[4];
extern const unsigned char cjk_fb_z[];   /* common JIS/GB/KS fallback, inflated on first miss */
extern const unsigned cjk_fb_zlen, cjk_fb_rawlen;
void gfx_cjk_select(int set);        /* 0 = none (Latin UI): only the language names can be drawn */

extern u32 *canvas;                  /* 0x00RRGGBB */
void gfx_init(void);
void gfx_present(void);
void gfx_clip(int x, int y, int w, int h);   /* recorte para todas las primitivas */
void gfx_noclip(void);
void gfx_clear(u32 c);
void gfx_rect(int x, int y, int w, int h, u32 c);
void gfx_rect_a(int x, int y, int w, int h, u32 c, int a);           /* a = 0..255 */
void gfx_vgrad(int x, int y, int w, int h, u32 top, u32 bot);
void gfx_rrect(int x, int y, int w, int h, int r, u32 c, int a);     /* redondeado AA */
void gfx_rframe(int x, int y, int w, int h, int r, int t, u32 c, int a);
void gfx_circle(int cx, int cy, int r, u32 c, int a);
void gfx_ring(int cx, int cy, int r, int t, u32 c, int a);
void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2, u32 c);
void gfx_line(int x0, int y0, int x1, int y1, int th, u32 c);
void gfx_blit(const u32 *px, int pw, int ph, int x, int y);          /* imagen RGB sin escala */
int  gfx_text(const Font *f, int x, int y, const char *utf8, u32 c);
int  gfx_text_w(const Font *f, const char *utf8);
/* texto recortado a maxw pixeles con "..." */
int  gfx_text_fit(const Font *f, int x, int y, const char *utf8, u32 c, int maxw);
void gfx_text_c(const Font *f, int cx, int y, const char *utf8, u32 c);
int  utf8_next(const char **s);
u32  gfx_mix(u32 a, u32 b, int t);   /* t 0..255 */
void gfx_blit_round(const u32 *px, int pw, int ph, int x, int y, int r);   /* r = pw/2: circulo */
void gfx_blit_scaled(const u32 *px, int pw, int ph, int x, int y, int w, int h);
/* filas de UI actualizadas sin redibujar todo (emotes animados) */
void gfx_ui_mark_rect(int x0, int y0, int x1, int y1);
int  gfx_ui_commit_rows(void);
void gfx_ui_cache_invalidate(void);
void gfx_black_now(void);
void gfx_get_clip(int *x0, int *y0, int *x1, int *y1);

/* iconos dibujados con primitivas (arte propio) */
enum { IC_NONE = 0, IC_NOTE, IC_SEARCH, IC_SD, IC_MENU, IC_PLAY, IC_PAUSE, IC_NEXT, IC_PREV,
       IC_STOP, IC_VOLUP, IC_VOLDN, IC_UP, IC_DOWN, IC_BACK, IC_OK, IC_SAVE, IC_EQ, IC_HEART, IC_USER, IC_LOCK, IC_CHAT };
void gfx_icon(int icon, int cx, int cy, int s, u32 c);
void gfx_blit_scaled_round(const u32 *p, int pw, int ph, int x, int y, int w, int h, int r);
#endif

