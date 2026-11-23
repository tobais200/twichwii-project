/* emote_ui.c - rc13/rc7: mensajes de chat con emotes como imagenes en linea.
 * Mientras la imagen no llego se muestra el texto (":KEKW:" o "Kappa") y despues
 * se reemplaza; el ajuste de lineas se recalcula cada cuadro con el ancho real. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tv.h"
#include "gfx.h"
#include <ogc/lwp_watchdog.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#define EH 18          /* alto del emote dibujado */
#define LH_EMO 20      /* alto de linea si hay emotes cargados */
#define LH_TXT 15
#define MAXT 160
typedef struct { const char *s; int n, w, kind; const kimg_t *im; const kanim_t *an; u32 col; } tok_t;   /* kind 0 texto 1 espacio 2 emote 3 nombre */

/* ---- rc14: emotes animados visibles ----
 * Cada emote animado dibujado en el ultimo redibujado completo del chat se anota aca
 * (posicion, recorte, color de fondo). chat_anim_tick() avanza los cuadros segun sus
 * demoras reales y sólo reescribe esas filas (gfx_ui_mark_rect/commit). La lista se
 * vacia en cada redibujado completo y en thumbs_flush, antes de que se libere nada. */
#define MAXV 40
typedef struct { const kanim_t *a; short x, y, cx0, cy0, cx1, cy1; int last; u32 bg; } vis_t;
static vis_t vis[MAXV];
static int nvis;
static u32 anim_last_tick;
static unsigned anim_rows, anim_ticks;
void thumbs_anim_gc(void);
static u32 ms_now(void) { return (u32)ticks_to_millisecs(gettime()); }
static int anim_frame(const kanim_t *a, u32 t) {
    if (a->total <= 0) return 0;
    u32 r = t % (u32)a->total;
    for (int i = 0; i < a->n; i++) { if (r < a->dly[i]) return i; r -= a->dly[i]; }
    return a->n - 1;
}
static void anim_blit(const vis_t *v, int f) {
    const kanim_t *a = v->a;
    const u16 *s = a->px + (size_t)f * a->w * a->h;
    for (int j = 0; j < a->h; j++) {
        int yy = v->y + j;
        if (yy < v->cy0 || yy >= v->cy1) continue;
        u32 *d = canvas + yy * SCR_W;
        for (int i = 0; i < a->w; i++) {
            int xx = v->x + i;
            if (xx < v->cx0 || xx >= v->cx1) continue;
            u16 c = s[j * a->w + i];
            d[xx] = c == ANIM_KEY ? v->bg : (((u32)(c >> 11) * 255 / 31) << 16) | (((u32)((c >> 5) & 63) * 255 / 63) << 8) | ((u32)(c & 31) * 255 / 31);
        }
    }
}
void chat_anim_reset(void) { nvis = 0; }
/* dibuja el cuadro actual; si hay lugar en la lista, queda animandose */
static void anim_place(const kanim_t *a, int x, int y) {
    vis_t v; int x0, y0, x1, y1;
    gfx_get_clip(&x0, &y0, &x1, &y1);
    v.a = a; v.x = x; v.y = y; v.cx0 = x0; v.cy0 = y0; v.cx1 = x1; v.cy1 = y1;
    int sx = x < x0 ? x0 : x, sy = y < y0 ? y0 : y;
    v.bg = (sx < SCR_W && sy < SCR_H && sx >= 0 && sy >= 0) ? canvas[sy * SCR_W + sx] : C_BG2;
    v.last = anim_frame(a, ms_now());
    anim_blit(&v, v.last);
    if (nvis < MAXV && x + a->w > x0 && x < x1 && y + a->h > y0 && y < y1) vis[nvis++] = v;
}
/* Llamar con el chat visible y sin redibujado completo. Devuelve filas reescritas. */
#define ANIM_PER_TICK 10
int chat_anim_tick(void) {
    u32 t = ms_now();
    /* <= 12,5 cuadros/s; con muchos emotes visibles baja a ~8 cuadros/s y actualiza como
     * maximo ANIM_PER_TICK por tick (en ronda), para que la conversion de la UI no le robe
     * tiempo al video (soak rc14: 30+ animados visibles a 360p bajaban el video). */
    static int rr;
    if (!nvis || t - anim_last_tick < (nvis > ANIM_PER_TICK ? 120u : 80u)) return 0;
    anim_last_tick = t;
    int done = 0;
    if (rr >= nvis) rr = 0;
    for (int n = 0, i = rr; n < nvis && done < ANIM_PER_TICK; n++, i = (i + 1) % nvis) {
        rr = (i + 1) % nvis;
        int f = anim_frame(vis[i].a, t);
        if (f == vis[i].last) continue;
        done++;
        vis[i].last = f;
        anim_blit(&vis[i], f);
        {
            const vis_t *v = &vis[i];
            int x0 = v->x < v->cx0 ? v->cx0 : v->x, x1 = v->x + v->a->w > v->cx1 ? v->cx1 : v->x + v->a->w;
            int y0 = v->y < v->cy0 ? v->cy0 : v->y, y1 = v->y + v->a->h > v->cy1 ? v->cy1 : v->y + v->a->h;
            gfx_ui_mark_rect(x0, y0, x1, y1);
        }
    }
    int r = gfx_ui_commit_rows();
    anim_rows += r; anim_ticks++;
    return r;
}
void chat_anim_stats(unsigned *rows, unsigned *ticks) { *rows = anim_rows; *ticks = anim_ticks; }
int chat_anim_count(void) { return nvis; }
/* al empezar un redibujado completo del panel de chat */
void chat_anim_begin(void) { nvis = 0; thumbs_anim_gc(); }

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

static int rich_static, rich_w;   /* rc14: vista previa del teclado: sin animacion; ancho medido */
int chat_rich(const void *fp, int x, int y, int maxw, const tchat_t *m, u32 namecol, u32 textcol, int draw) {
    const Font *f = (const Font *)fp;
    static tok_t t[MAXT];
    int nt = 0, pos = 0, e = 0, any = 0, len = strlen(m->text), i;
    char url[160];
    if (m->name[0] && !m->sys) {
        t[nt++] = (tok_t){ m->name, (int)strlen(m->name), gfx_text_w(f, m->name) + 1, 3, NULL, NULL, namecol };
        t[nt++] = (tok_t){ ":", 1, gfx_text_w(f, ":"), 0, NULL, NULL, textcol };
        t[nt++] = (tok_t){ " ", 1, gfx_text_w(f, " "), 1, NULL, NULL, textcol };
    }
    int spw = gfx_text_w(f, " ");
    while (pos < len && nt < MAXT) {
        while (e < m->ne && m->eat[e] < pos) e++;
        if (e < m->ne && m->eat[e] == pos && m->elen[e] > 0 && pos + m->elen[e] <= len) {
            const kimg_t *im = NULL; const kanim_t *an = NULL;
            if (emote_url(m->eid[e], url, sizeof(url))) im = rich_static ? emote_get(url) : emote_get_anim(url, &an);
            if (!im) an = NULL;
            int w = an ? an->w : im && im->h > 0 ? im->w * EH / im->h : sub_w(f, m->text + pos, m->elen[e]);
            if (im) any = 1;
            t[nt++] = (tok_t){ m->text + pos, m->elen[e], w, 2, im, an, textcol };
            pos += m->elen[e]; e++;
            continue;
        }
        if (m->text[pos] == ' ') { t[nt++] = (tok_t){ m->text + pos, 1, spw, 1, NULL, NULL, textcol }; pos++; continue; }
        int stop = (e < m->ne && m->eat[e] > pos) ? m->eat[e] : len, k = pos;
        while (k < stop && m->text[k] != ' ') k++;
        t[nt++] = (tok_t){ m->text + pos, k - pos, sub_w(f, m->text + pos, k - pos), 0, NULL, NULL, textcol };
        pos = k;
    }
    int lh = any ? LH_EMO : LH_TXT, ty = any ? 3 : 0, cx = 0, line = 0;
    rich_w = 0;
    for (i = 0; i < nt; i++) {
        tok_t *k = &t[i];
        if (k->kind == 1 && cx == 0 && line > 0) continue;            /* sin espacio al comenzar linea */
        if (cx + k->w > maxw && cx > 0) {
            line++; cx = 0;
            if (k->kind == 1) continue;
        }
        int yy = y + line * lh;
        if (k->kind == 2 && k->im) {
            if (draw && k->an) anim_place(k->an, x + cx, yy + (lh - EH) / 2);
            else if (draw) gfx_blit_scaled(k->im->px, k->im->w, k->im->h, x + cx, yy + (lh - EH) / 2, k->w, EH);
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
        if (cx > rich_w) rich_w = cx;
    }
    return (line + 1) * lh;
}

/* ---- rc14: campo de texto del teclado con los emotes como imagenes (estaticas) ----
 * Una sola linea; si no entra se muestra el final. Devuelve la x del cursor. */
int emote_preview_draw(const tchat_t *m, int x, int y, int w) {
    rich_static = 1;
    chat_rich(&font_title, 0, 0, 1 << 20, m, C_TEXT, C_TEXT, 0);
    int tw = rich_w, sh = tw > w ? tw - w : 0;
    int x0, y0, x1, y1; gfx_get_clip(&x0, &y0, &x1, &y1);
    gfx_clip(x, y - 6, w + 4, 34);
    chat_rich(&font_title, x - sh, y - 3, 1 << 20, m, C_TEXT, C_TEXT, 1);
    gfx_clip(x0, y0, x1 - x0, y1 - y0);
    rich_static = 0;
    return x - sh + tw + 2;
}

/* ---- rc14: grilla de emotes (selector del teclado del chat) ----
 * Imagenes estaticas (primer cuadro) por el mismo cache acotado de thumbs.c.
 * Puntero o cruz para elegir, A inserta, B cierra, +/- o pestañas cambian de grupo. */
int emote_grid(const char *title, const emo_item_t *it, int n, const char *const *gname, int ng, int *sel_io) {
    int px = (SCR_W - 600) / 2, py = 30, pw = 600, ph = 420;
    int cell = 50, cols = (pw - 70) / cell, rows = 5;
    int gx = px + 20, gy = py + 102;
    static int grp = 0, top = 0;
    int sel = sel_io ? *sel_io : 0, res = -1, done = 0, i;
    int idx[400], cnt[8] = { 0 };
    for (i = 0; i < n; i++) if (it[i].group < 8) cnt[it[i].group]++;
    if (grp >= ng || !cnt[grp]) for (grp = 0; grp < ng && !cnt[grp]; grp++) ;
    if (grp >= ng) return -1;
    { int k; for (k = 0; k < 2; k++) { in_poll(); VIDEO_WaitVSync(); } }
    while (SYS_MainLoop() && !done) {
        u32 d = in_poll();
        int key, m = 0, hov_any = 0;
        ui_begin(d);
        thumbs_frame();
        while ((key = in_key()) != 0) { if (key == 27) done = 1; else if (key == 13) d |= WPAD_BUTTON_A; }
        for (i = 0; i < n && m < 400; i++) if (it[i].group == grp) idx[m++] = i;
        if (sel >= m) sel = m - 1;
        if (sel < 0) sel = 0;
        if (d & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) done = 1;
        int ng2 = grp;
        if (d & WPAD_BUTTON_PLUS) { do ng2 = (ng2 + 1) % ng; while (!cnt[ng2] && ng2 != grp); }
        if (d & WPAD_BUTTON_MINUS) { do ng2 = (ng2 + ng - 1) % ng; while (!cnt[ng2] && ng2 != grp); }
        if (d & WPAD_BUTTON_RIGHT) sel = sel + 1 < m ? sel + 1 : sel;
        if (d & WPAD_BUTTON_LEFT) sel = sel > 0 ? sel - 1 : 0;
        if (d & WPAD_BUTTON_DOWN) sel = sel + cols < m ? sel + cols : m - 1;
        if (d & WPAD_BUTTON_UP) sel = sel - cols >= 0 ? sel - cols : sel;
        int maxtop = (m + cols - 1) / cols - rows; if (maxtop < 0) maxtop = 0;
        if (sel / cols < top) top = sel / cols;
        if (sel / cols >= top + rows) top = sel / cols - rows + 1;
        if (top > maxtop) top = maxtop;

        ui_backdrop(170);
        ui_panel(px, py, pw, ph, title);
        /* pestañas */
        int tx = px + 20, tw = (pw - 40 - (ng - 1) * 6) / ng;
        for (i = 0; i < ng; i++) {
            char lab[48]; snprintf(lab, sizeof lab, "%s (%d)", gname[i], cnt[i]);
            if (!cnt[i]) { gfx_rrect(tx, py + 56, tw, 32, 8, C_BG2, 255); gfx_text_c(&font_small, tx + tw / 2, py + 64, lab, C_FAINT); }
            else if (ui_button(tx, py + 56, tw, 32, lab, 0, i == grp ? BS_ACCENT : BS_NORMAL, 0)) ng2 = i;
            tx += tw + 6;
        }
        /* grilla */
        gfx_rrect(gx - 6, gy - 6, cols * cell + 12, rows * cell + 12, 10, 0x0B0B0E, 255);
        for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
            int k = (top + r) * cols + c;
            if (k >= m) break;
            const emo_item_t *e = &it[idx[k]];
            int x = gx + c * cell, y = gy + r * cell;
            int hov = ui_hover(x, y, cell - 2, cell - 2);
            if (hov) { sel = k; hov_any = 1; }
            if (k == sel) { gfx_rrect(x, y, cell - 2, cell - 2, 8, C_PANEL2, 255); gfx_rframe(x, y, cell - 2, cell - 2, 8, 2, C_ACCENT, 255); }
            char url[200]; const kimg_t *im = emote_url(e->id, url, sizeof url) ? emote_get(url) : NULL;
            if (im && im->h > 0) {
                int h = 32, w = im->w * 32 / im->h; if (w > cell - 8) { w = cell - 8; h = im->h * w / im->w; }
                gfx_blit_scaled(im->px, im->w, im->h, x + (cell - 2 - w) / 2, y + (cell - 2 - h) / 2, w, h);
            } else {
                gfx_text_fit(&font_small, x + 4, y + 16, e->name, C_FAINT, cell - 10);
            }
            if (e->locked) { gfx_rect_a(x + 2, y + 2, cell - 6, cell - 6, 0, 110); gfx_icon(IC_LOCK, x + cell - 13, y + 11, 12, C_WARN); }
            if (hov && (d & WPAD_BUTTON_A) && !UI.consumed) { res = idx[k]; done = 1; }
        }
        /* desplazamiento con el puntero */
        int ax = gx + cols * cell + 12;
        if (top > 0 && ui_button(ax, gy, 30, 60, "", IC_UP, BS_NORMAL, 0)) top--;
        if (top < maxtop && ui_button(ax, gy + rows * cell - 60, 30, 60, "", IC_DOWN, BS_NORMAL, 0)) top++;
        if (m > rows * cols) {
            int bh = rows * cell - 130, th = bh * rows / ((m + cols - 1) / cols); if (th < 12) th = 12;
            gfx_rect(ax + 13, gy + 65, 4, bh, C_BG2);
            gfx_rect(ax + 13, gy + 65 + (maxtop ? (bh - th) * top / maxtop : 0), 4, th, C_DIM);
        }
        /* elegido */
        if (m > 0) {
            const emo_item_t *e = &it[idx[sel]];
            char l[120];
            snprintf(l, sizeof l, "%s%s", e->name, e->locked ? "   (solo suscriptores)" : e->note[0] ? "   " : "");
            if (!e->locked && e->note[0]) snprintf(l + strlen(l), sizeof l - strlen(l), "(%s)", e->note);
            gfx_text_fit(&font_bold, px + 22, py + ph - 52, l, e->locked ? C_WARN : C_TEXT, pw - 44);
        }
        gfx_text_c(&font_small, SCR_W / 2, py + ph - 22, "A: insertar   B: cerrar   +/-: grupo   cruz: mover", C_FAINT);
        ui_present();
        if (!done && (d & WPAD_BUTTON_A) && !UI.consumed && m > 0 && !hov_any) { res = idx[sel]; done = 1; }
        if (ng2 != grp) { grp = ng2; sel = 0; top = 0; }
    }
    if (sel_io) *sel_io = sel;
    { int k; for (k = 0; k < 2; k++) { in_poll(); VIDEO_WaitVSync(); } }
    return res;
}
