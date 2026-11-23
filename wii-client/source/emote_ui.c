/* emote_ui.c - rc13/rc7: mensajes de chat con emotes como imagenes en linea.
 * Mientras la imagen no llego se muestra el texto (":KEKW:" o "Kappa") y despues
 * se reemplaza; el ajuste de lineas se recalcula cada cuadro con el ancho real. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tv.h"
#include "gfx.h"

#define EH 18          /* alto del emote dibujado */
#define LH_EMO 20      /* alto de linea si hay emotes cargados */
#define LH_TXT 15
#define MAXT 160
typedef struct { const char *s; int n, w, kind; const kimg_t *im; u32 col; } tok_t;   /* kind 0 texto 1 espacio 2 emote 3 nombre */

static int sub_w(const Font *f, const char *s, int n) {
    char b[208]; if (n > 207) n = 207;
    memcpy(b, s, n); b[n] = 0; return gfx_text_w(f, b);
}
static void sub_draw(const Font *f, int x, int y, const char *s, int n, u32 c, int bold) {
    char b[208]; if (n > 207) n = 207;
    memcpy(b, s, n); b[n] = 0; gfx_text(f, x, y, b, c); if (bold) gfx_text(f, x + 1, y, b, c);
}
static int u8len(const char *s) {
    unsigned char c = (unsigned char)*s;
    return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
}

int chat_rich(const void *fp, int x, int y, int maxw, const tchat_t *m, u32 namecol, u32 textcol, int draw) {
    const Font *f = (const Font *)fp;
    static tok_t t[MAXT];
    int nt = 0, pos = 0, e = 0, any = 0, len = strlen(m->text), i;
    char url[160];
    if (m->name[0] && !m->sys) {
        t[nt++] = (tok_t){ m->name, (int)strlen(m->name), gfx_text_w(f, m->name) + 1, 3, NULL, namecol };
        t[nt++] = (tok_t){ ":", 1, gfx_text_w(f, ":"), 0, NULL, textcol };
        t[nt++] = (tok_t){ " ", 1, gfx_text_w(f, " "), 1, NULL, textcol };
    }
    int spw = gfx_text_w(f, " ");
    while (pos < len && nt < MAXT) {
        while (e < m->ne && m->eat[e] < pos) e++;
        if (e < m->ne && m->eat[e] == pos && m->elen[e] > 0 && pos + m->elen[e] <= len) {
            const kimg_t *im = NULL;
            if (emote_url(m->eid[e], url, sizeof(url))) im = emote_get(url);
            int w = im && im->h > 0 ? im->w * EH / im->h : sub_w(f, m->text + pos, m->elen[e]);
            if (im) any = 1;
            t[nt++] = (tok_t){ m->text + pos, m->elen[e], w, 2, im, textcol };
            pos += m->elen[e]; e++;
            continue;
        }
        if (m->text[pos] == ' ') { t[nt++] = (tok_t){ m->text + pos, 1, spw, 1, NULL, textcol }; pos++; continue; }
        int stop = (e < m->ne && m->eat[e] > pos) ? m->eat[e] : len, k = pos;
        while (k < stop && m->text[k] != ' ') k++;
        t[nt++] = (tok_t){ m->text + pos, k - pos, sub_w(f, m->text + pos, k - pos), 0, NULL, textcol };
        pos = k;
    }
    int lh = any ? LH_EMO : LH_TXT, ty = any ? 3 : 0, cx = 0, line = 0;
    for (i = 0; i < nt; i++) {
        tok_t *k = &t[i];
        if (k->kind == 1 && cx == 0 && line > 0) continue;            /* sin espacio al comenzar linea */
        if (cx + k->w > maxw && cx > 0) {
            line++; cx = 0;
            if (k->kind == 1) continue;
        }
        int yy = y + line * lh;
        if (k->kind == 2 && k->im) {
            if (draw) gfx_blit_scaled(k->im->px, k->im->w, k->im->h, x + cx, yy + (lh - EH) / 2, k->w, EH);
            cx += k->w + 1;
            continue;
        }
        if (k->w > maxw) {   /* palabra mas larga que el panel: cortar por caracteres */
            const char *s = k->s, *end = k->s + k->n;
            while (s < end) {
                const char *q = s; int w = 0;
                while (q < end) { int cl = u8len(q); int nw = sub_w(f, s, (int)(q - s) + cl); if (nw + cx > maxw && q > s) break; w = nw; q += cl; }
                if (draw) sub_draw(f, x + cx, y + line * lh + ty, s, (int)(q - s), k->col, k->kind == 3);
                cx += w; s = q;
                if (s < end) { line++; cx = 0; }
            }
            continue;
        }
        if (draw && k->kind != 1) sub_draw(f, x + cx, yy + ty, k->s, k->n, k->col, k->kind == 3);
        cx += k->w;
    }
    return (line + 1) * lh;
}
