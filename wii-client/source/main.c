/*
 * TwitchWii - cliente NO OFICIAL (fan-made) de twitch.tv para la Wii (Homebrew Channel).
 * Interfaz al estilo twitch.tv: barra superior (logo, Inicio / Explorar, buscador),
 * "Canales recomendados" a la izquierda, "Canales en vivo que creemos que te gustaran"
 * con tarjetas (miniatura, EN VIVO rojo, espectadores, avatar, titulo, categoria,
 * etiquetas) y efecto hover violeta, Explorar por categorias (box art), pagina de canal
 * con videos recientes, y reproductor con chat de solo lectura a la derecha (o pantalla
 * completa). Datos: servidor companion en la LAN o directo a gql.twitch.tv.
 * Datos publicos con login opcional. No afiliado a Twitch ni a Nintendo.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <curl/curl.h>
#include "tv.h"
#include "guard.h"
#include "dlog.h"
#include "social.h"
#include "mem2.h"
static int chat_write_pressed,watch_social_pressed;

/* ---------------------------------------------------------------- layout (640/854x480) */
#define TOP_H    44
#define SB_W     150
#define MX       (SB_W + 12)
#define MW       (SCR_W - MX - 10)
#define HDR_Y    (TOP_H + 10)
#define CHIP_Y   (TOP_H + 38)
#define GRID_Y_BASE (TOP_H + 66)
#define SHELF_H     166
#define GRID_Y      (GRID_Y_BASE + grid_y_extra)
#define GRID_H      (SCR_H - GRID_Y)
#define VIEW_Y      GRID_Y_BASE          /* scroll viewport: below chips, includes Top categorias */
#define VIEW_H      (SCR_H - VIEW_Y)
#define CW       ((MW-(COLS-1)*CGAP_X)/COLS)
#define TH       (CW*9/16)
#define CH       (TH + 78)
#define CGAP_X   20
#define CGAP_Y   12
#define COLS     (gfx_widescreen()?3:2)
#define MORE_H   34
/* explorar */
#define BW       96
#define BH       128
#define BCOLS    (gfx_widescreen()?6:4)
#define BGAP     ((MW-BCOLS*BW)/(BCOLS-1))
#define BCH      (BH + 44)

enum { SCR_HOME = 0, SCR_BROWSE, SCR_SEARCH, SCR_CHAN, SCR_WATCH };
static int screen = SCR_HOME, prev_screen = SCR_HOME;
static int fat_ok = 0, quit = 0;

static klist_t live;                 /* feed en vivo (o de una categoria) */
static char live_after[200] = "";
static int live_more = 0, live_loaded = 0;
static char live_err[220] = "";
static int *flt = NULL, nflt = 0, nhidden = 0;
static char cats[8][48]; static int ncats = 0, cat_sel = 0;
static int sel = 0;                  /* indice en flt (o "mostrar mas" = nflt) */
static int scroll = 0, scroll_t = 0;
static char cur_game[48] = "", cur_game_name[48] = "", cur_game_box[768] = "";
static int cur_game_viewers = 0;
static klist_t side;                 /* recomendados (barra lateral): siempre el feed general */

static tcatlist_t bcats;             /* explorar (paginado, heap) */
static int bsel = 0, bscroll = 0, bscroll_t = 0, bloaded = 0, bcats_more = 0, bcats_loading = 0;
static char berr[220] = "";
static char cat_search_q[64] = "";
static tcat_t home_cats[12];
static int nhome_cats = 0;
static int grid_y_extra = 0;
static tcatlist_t search_cats;

static klist_t results;              /* busqueda */
static char search_q[48] = "";
static int rsel = 0, rscroll = 0;
static char search_err[220] = "";

static kstream_t chan;               /* canal abierto */
static tvod_t vods[8]; static int nvods = 0;
static char chan_err[220] = "";
static int chan_focus = 0;           /* 0 ver, 1 chat, 2 modo, 3 volver, 4.. videos */

static kstream_t pinfo;              /* canal que se esta viendo */
static tvod_t pvod; static int watching_vod = 0;
static int playing = 0;
/* Presentation state is independent of the chat connection/config. */
static int watch_details = 0, watch_chat_started = 0;
static u64 osd_t = 0;
static const tv_pic_t *curpic = NULL;
static long k_time_off = 0;          /* reloj UTC del servidor - reloj de la Wii */
static FILE *atlog = NULL;

static u64 t_start = 0;
static int at_block = 0;
#define LOG(...) do { if (atlog) { fprintf(atlog, "[%6.1fs] ", t_start ? ticks_to_millisecs(gettime() - t_start) / 1000.0 : 0.0); fprintf(atlog, __VA_ARGS__); fputc('\n', atlog); fflush(atlog); } } while (0)
#define BUSY(x) do { at_block = 1; x; at_block = 0; } while (0)

/* ---------------------------------------------------------------- utilidades */
static void fmt_count(int n, char *o, int sz) {
    if (n >= 1000000) snprintf(o, sz, "%d,%d M", n / 1000000, (n / 100000) % 10);
    else if (n >= 10000) snprintf(o, sz, "%d mil", n / 1000);
    else if (n >= 1000) snprintf(o, sz, "%d,%d mil", n / 1000, (n / 100) % 10);
    else snprintf(o, sz, "%d", n);
}
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static long secs_since(const char *start) {
    int Y, M, D, h, m, s;
    if (!k_server_ok || !start[0] || sscanf(start, "%d-%d-%d%*c%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return -1;
    long t0 = days_from_civil(Y, M, D) * 86400L + h * 3600 + m * 60 + s;
    return (long)sh_clock_now() + k_time_off - t0;
}
/* tiempo en vivo estilo Twitch: 2:15:03 */
static void fmt_uptime(const char *start, char *o, int sz) {
    long t = secs_since(start);
    o[0] = 0;
    if (t < 0 || t > 3600L * 96) return;
    snprintf(o, sz, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
}
static void fmt_ago(const char *start, char *o, int sz) {
    long t = secs_since(start);
    o[0] = 0;
    if (t < 0) return;
    if (t < 3600) snprintf(o, sz, "hace %ld min", t / 60);
    else if (t < 86400) snprintf(o, sz, "hace %ld h", t / 3600);
    else if (t < 86400L * 60) snprintf(o, sz, "hace %ld d\xc3\xad" "as", t / 86400);
    else snprintf(o, sz, "hace %ld meses", t / (86400L * 30));
}
static void fmt_dur(int s, char *o, int sz) {
    if (s >= 3600) snprintf(o, sz, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(o, sz, "%d:%02d", s / 60, s % 60);
}

static int hidden_by_filter(const kstream_t *s) { return !k_show_mature && (s->mature || s->gambling); }

static void rebuild_filter(void) {
    int i, j;
    int *replacement = malloc(sizeof(int) * (live.n + 1));
    int *previous = flt; flt = replacement; free(previous);
    nflt = 0; nhidden = 0;
    if (!flt || (live.n && !live.v)) {
        ncats = cat_sel = sel = 0;
        ui_toast("Sin memoria para filtrar la lista. Reintenta actualizar.");
        return;
    }
    ncats = 0;
    if (!cur_game[0]) for (i = 0; i < live.n && ncats < 8; i++) {
        if (hidden_by_filter(&live.v[i]) || !live.v[i].cat[0]) continue;
        for (j = 0; j < ncats; j++) if (!strcmp(cats[j], live.v[i].cat)) break;
        if (j == ncats) snprintf(cats[ncats++], sizeof(cats[0]), "%s", live.v[i].cat);
    }
    if (cat_sel < 0 || cat_sel > ncats) cat_sel = 0;
    for (i = 0; i < live.n; i++) {
        if (hidden_by_filter(&live.v[i])) { nhidden++; continue; }
        if (cat_sel && strcmp(live.v[i].cat, cats[cat_sel - 1])) continue;
        flt[nflt++] = i;
    }
    if (sel < 0) sel = 0;
    if (sel > nflt) sel = nflt;
}

/* Language table is static; keep all three entry points in sync. */
static void next_language(void) {
    if (k_lang < 0 || k_lang >= k_nlangs) k_lang = 0;
    k_lang = (k_lang + 1) % k_nlangs;
    sel = scroll = scroll_t = rsel = rscroll = 0;
    cfg_save();
}
static const kstream_t *filtered_stream(int i) {
    if (!flt || !live.v || i < 0 || i >= nflt || flt[i] < 0 || flt[i] >= live.n) return NULL;
    return &live.v[flt[i]];
}

/* ---------------------------------------------------------------- red (a pedido) */
static void load_live(int reset) {
    ui_backdrop_suspend(1);
    char err[220] = "";
    int more = 0;
    sui_busy("TwitchWii", reset ? (cur_game[0] ? "Cargando directos de la categor\xc3\xad" "a..." : "Cargando canales en vivo...") : "Cargando m\xc3\xa1s canales...");
    if (reset) {
        thumbs_flush();
        /* busy_draw()/curl progress can re-enter draw_home(): never leave stale
           filter indices pointing into a cleared or partially rebuilt list. */
        free(flt); flt = NULL; nflt = nhidden = ncats = 0;
        live_more = 0; cat_sel = sel = 0; scroll = scroll_t = 0;
        klist_clear(&live); live_after[0] = 0;
    }
    int r; BUSY(r = tw_live(cur_game, live_after, sizeof(live_after), &live, &more, err, sizeof(err)));
    live_loaded = 1;
    live_more = r > 0 && more;
    snprintf(live_err, sizeof(live_err), "%s", r < 0 ? err : "");
    if (r < 0) ui_toast(err);
    LOG("live[%s]: r=%d n=%d more=%d mock=%d err=%s", cur_game, r, live.n, live_more, k_last_mock, err);
    if (!cur_game[0] && r > 0 && reset) {      /* la barra lateral sigue el feed general */
        int i; klist_clear(&side);
        for (i = 0; i < live.n && i < 14; i++) { kstream_t *s = klist_add(&side); if (s) *s = live.v[i]; }
    }
    rebuild_filter();
    sui_busy_end();
    ui_backdrop_suspend(0);
}

static void load_home_cats(void) {
    char err[220] = "";
    int r, i;
    if (bcats.n > 0) {
        nhome_cats = bcats.n < 12 ? bcats.n : 12;
        for (i = 0; i < nhome_cats; i++) home_cats[i] = bcats.v[i];
        return;
    }
    r = tw_top_categories(home_cats, 12, err, sizeof(err));
    nhome_cats = r > 0 ? r : 0;
    LOG("home_cats: r=%d err=%s", r, err);
}

static void load_browse(void) {
    char err[220] = "";
    int more = 0, r;
    sui_busy("Explorar", "Cargando categor\xc3\xad" "as...");
    cat_search_q[0] = 0;
    tw_categories_reset();
    tcatlist_clear(&bcats);
    bcats_more = 0;
    BUSY(r = tw_categories_page(&bcats, &more, err, sizeof(err)));
    sui_busy_end();
    bloaded = 1; bsel = 0; bscroll = bscroll_t = 0;
    bcats_more = r > 0 && more;
    snprintf(berr, sizeof(berr), "%s", r < 0 ? err : "");
    if (r < 0) ui_toast(err);
    if (bcats.n && !nhome_cats) load_home_cats();
    LOG("categories: r=%d n=%d more=%d err=%s", r, bcats.n, bcats_more, err);
}

static void load_browse_more(void) {
    char err[220] = "";
    int more = 0, r;
    if (!bcats_more || bcats_loading || cat_search_q[0]) return;
    bcats_loading = 1;
    sui_busy("Explorar", "Cargando m\xc3\xa1s categor\xc3\xad" "as...");
    BUSY(r = tw_categories_page(&bcats, &more, err, sizeof(err)));
    sui_busy_end();
    bcats_more = r > 0 && more;
    if (r < 0) { snprintf(berr, sizeof(berr), "%s", err); ui_toast(err); }
    bcats_loading = 0;
    LOG("categories more: r=%d n=%d more=%d err=%s", r, bcats.n, bcats_more, err);
}

static void do_cat_search(void) {
    char q[64], err[220] = "";
    int r;
    snprintf(q, sizeof(q), "%s", cat_search_q);
    if (!sui_text_input("Buscar categor\xc3\xad" "a", "Nombre del juego o categor\xc3\xad" "a. +: buscar", q, sizeof(q) - 1)) return;
    { char *s = q; int n; while (*s == ' ') s++; n = strlen(s); while (n && s[n - 1] == ' ') s[--n] = 0; if (!*s) return;
      snprintf(cat_search_q, sizeof(cat_search_q), "%s", s); }
    sui_busy("Explorar", "Buscando categor\xc3\xad" "as...");
    tw_categories_reset();
    tcatlist_clear(&bcats);
    BUSY(r = tw_search_categories(cat_search_q, &bcats, err, sizeof(err)));
    sui_busy_end();
    bloaded = 1; bsel = 0; bscroll = bscroll_t = 0; bcats_more = 0;
    snprintf(berr, sizeof(berr), "%s", r < 0 ? err : "");
    if (r < 0) ui_toast(err);
    screen = SCR_BROWSE;
    LOG("cat_search '%s': r=%d n=%d err=%s", cat_search_q, r, bcats.n, err);
}

static void open_category(const tcat_t *c) {
    if (c->slug[0]) snprintf(cur_game, sizeof(cur_game), "%s", c->slug);
    else if (c->id[0]) snprintf(cur_game, sizeof(cur_game), "id:%s", c->id);
    else return;
    snprintf(cur_game_name, sizeof(cur_game_name), "%s", c->name);
    snprintf(cur_game_box, sizeof(cur_game_box), "%s", c->boxart);
    cur_game_viewers = c->viewers;
    screen = SCR_HOME;
    load_live(1);
}

static void go_home(void) {
    int was = cur_game[0] != 0;
    cur_game[0] = 0;
    screen = SCR_HOME;
    if (was || !live_loaded) load_live(1);
}

static void check_server(void) {
    char err[200] = "";
    if (!k_server[0]) { k_server_ok = 0; return; }
    sui_busy("TwitchWii", "Conectando con el servidor companion...");
    int ok; BUSY(ok = tw_health(err, sizeof(err)));
    sui_busy_end();
    if (ok) {
        sh_resp_t r; char url[200];
        snprintf(url, sizeof(url), "%s/wii/health", k_server);
        if (sh_request("GET", url, NULL, NULL, NULL, &r) == 0 && r.data) {
            const char *v = jget(jws(r.data), "time");
            if (v) k_time_off = (long)jnum(v) - (long)sh_clock_now();
        }
        sh_resp_free(&r);
    } else ui_toast(err);
    LOG("health: ok=%d err=%s", ok, err);
}

static void open_channel(const kstream_t *from, const char *slug) {
    char err[220] = "";
    if (from) chan = *from; else { memset(&chan, 0, sizeof(chan)); snprintf(chan.slug, sizeof(chan.slug), "%s", slug); }
    sui_busy("TwitchWii", "Abriendo canal...");
    int r; BUSY(r = tw_channel(slug, &chan, vods, 8, &nvods, err, sizeof(err)));
    sui_busy_end();
    snprintf(chan_err, sizeof(chan_err), "%s", r < 0 ? err : "");
    if (r < 0 && !from) { ui_toast(err); return; }
    if (r < 0) nvods = 0;
    LOG("channel %s: r=%d live=%d viewers=%d cat=%s vods=%d err=%s", slug, r, chan.live, chan.viewers, chan.cat, nvods, err);
    chan_focus = chan.live > 0 ? 0 : (nvods ? 6 : 5);
    prev_screen = screen == SCR_CHAN ? prev_screen : screen;
    screen = SCR_CHAN;
}

static void warm_watch_images(void) {
    const char *paths[8];int n=0,i,ready;
    paths[n++]=chan.avatar;paths[n++]=chan.thumb;paths[n++]=chan.banner;
    for(i=0;i<nvods&&i<4;i++)paths[n++]=vods[i].thumb;
    ui_set_backdrop(NULL);sui_busy("TwitchWii","Preparando imagenes del directo...");
    thumbs_pause(0);at_block=1;u64 start=gettime();
    do {
        thumbs_frame();ready=0;
        for(i=0;i<n;i++)if(!paths[i][0]||thumb_get(paths[i]))ready++;
        ui_busy_tick();if(ready==n)break;usleep(50000);
    } while(ticks_to_millisecs(gettime()-start)<8000);
    LOG("image prefetch: %d/%d ready",ready,n);at_block=0;
    sui_busy_end();thumbs_pause(1);
}
static void start_watch(void) {
    warm_watch_images();
    intro_music_stop();
    static char url[4096];
    char refresh[300], err[220] = "";
    int max_h = 360;
    if (!tw_play_url(&chan, url, sizeof(url), refresh, sizeof(refresh), &max_h, err, sizeof(err))) { thumbs_pause(0); ui_toast(err); LOG("play: %s", err); return; }
    pinfo = chan; watching_vod = 0;
    tp_set_vod(0);
    tp_set_limits(max_h, refresh);
    tp_play(url, VM_AUTO);
    if(auth_due())auth_ensure();
    watch_details = k_chat_on; watch_chat_started = watch_details;
    if (watch_chat_started) chat_start(pinfo.slug);
    playing = 1;
    screen = SCR_WATCH;
    osd_t = gettime();
    LOG("play: channel=%s direct=%d",pinfo.slug,!k_server[0]);
}

static void start_vod(int i) {
    intro_music_stop();
    static char url[8192];
    char err[220] = "";
    if (i < 0 || i >= nvods) return;
    warm_watch_images();
    if (!tw_vod_url(&vods[i], url, sizeof(url), err, sizeof(err))) { thumbs_pause(0);ui_toast(err);LOG("vod rejected: %s",err);return; }
    pinfo = chan; pvod = vods[i]; watching_vod = 1;
    snprintf(pinfo.title, sizeof(pinfo.title), "%s", pvod.title);
    tp_set_vod(1);
    tp_set_limits(360, "");
    tp_play(url, VM_AUTO);
    chat_stop();
    playing = 1;
    screen = SCR_WATCH;
    osd_t = gettime();
    LOG("play vod %s direct=%d",pvod.id,!k_server[0]);
}

static void stop_watch(void) { chat_stop(); tp_stop(); playing = 0; thumbs_pause(0); }

static int valid_login(const char *s) {
    int i;
    if (!s[0] || strlen(s) > 25) return 0;
    for (i = 0; s[i]; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    return 1;
}

static void do_search(void) {
    char q[48];
    snprintf(q, sizeof(q), "%s", search_q);
    if (!sui_text_input("Buscar en Twitch", "Escrib\xc3\xad el nombre de un canal (twitch.tv/nombre). +: buscar", q, sizeof(q) - 1)) return;
    char *p = q; while (*p == ' ') p++;
    int n = strlen(p); while (n && p[n - 1] == ' ') p[--n] = 0;
    if (!*p) return;
    snprintf(search_q, sizeof(search_q), "%s", p);
    klist_clear(&results); tcatlist_clear(&search_cats); rsel = 0; rscroll = 0;
    char err[220] = "", cerr[220] = "";
    sui_busy("Buscar", "Buscando canales y categor\xc3\xad" "as...");
    int r, rc; BUSY(r = tw_search(search_q, &results, err, sizeof(err)));
    BUSY(rc = tw_search_categories(search_q, &search_cats, cerr, sizeof(cerr)));
    sui_busy_end();
    snprintf(search_err, sizeof(search_err), "%s", r < 0 ? err : "");
    if (rc < 0) rc = 0;
    LOG("search '%s': r=%d cats=%d err=%s", search_q, r, search_cats.n, err);
    screen = SCR_SEARCH;
}

/* ---------------------------------------------------------------- dibujo: piezas */
static void text_bold(const Font *f, int x, int y, const char *s, u32 c) { gfx_text(f, x, y, s, c); gfx_text(f, x + 1, y, s, c); }

static int live_badge(int x, int y) {          /* "EN VIVO" rojo (Twitch: LIVE) */
    int w = gfx_text_w(&font_small, "EN VIVO") + 12;
    gfx_rrect(x, y, w, 18, 4, C_LIVE, 255);
    text_bold(&font_small, x + 6, y + 1, "EN VIVO", 0xFFFFFF);
    return w;
}
static void offline_badge(int x, int y) {
    int w = gfx_text_w(&font_small, "DESCONECTADO") + 12;
    gfx_rrect(x, y, w, 18, 4, C_PANEL2, 255);
    text_bold(&font_small, x + 6, y + 1, "DESCONECTADO", C_DIM);
}
static void viewers_pill(int x, int y, int n, const char *suffix) {
    char v[24], t[48];
    fmt_count(n, v, sizeof(v));
    snprintf(t, sizeof(t), "%s%s", v, suffix);
    int w = gfx_text_w(&font_small, t) + 10;
    gfx_rrect(x, y, w, 18, 3, 0x000000, 170);
    gfx_text(&font_small, x + 5, y + 1, t, 0xFFFFFF);
}
static void partner_check(int cx, int cy, int r) {     /* insignia de verificado (violeta) */
    gfx_circle(cx, cy, r, C_ACCENT, 255);
    gfx_icon(IC_OK, cx, cy, r + r / 3, 0xFFFFFF);
}
static int tag_pill(int x, int y, const char *t, int maxw) {
    int w = gfx_text_w(&font_small, t) + 14;
    if (maxw && w > maxw) w = maxw;
    if (w < 24) return 0;
    gfx_rrect(x, y, w, 18, 9, C_TAG, 255);
    gfx_text_fit(&font_small, x + 7, y + 1, t, C_DIM, w - 12);
    return w;
}
static int chip(int x, int y, const char *t, int on, int maxw) {
    int w = gfx_text_w(&font_small, t) + 18;
    if (maxw && w > maxw) w = maxw;
    int hov = ui_hover(x, y, w, 22);
    gfx_rrect(x, y, w, 22, 11, on ? C_ACCENT : (hov ? 0x45454D : C_PANEL2), 255);
    gfx_text_fit(&font_small, x + 9, y + 3, t, on ? 0xFFFFFF : C_TEXT, w - 16);
    return w;
}

static void placeholder(const kstream_t *s, int x, int y, int w, int h, const Font *f) {
    gfx_vgrad(x, y, w, h, gfx_mix(s->color, C_BG, 60), gfx_mix(s->color, C_BG, 210));
    gfx_text_c(f, x + w / 2, y + h / 2 - f->line_h / 2 - 4, s->shortn, 0xFFFFFF);
    if (h > 60) {
        char t[80]; snprintf(t, sizeof(t), "twitch.tv/%s", s->slug);
        gfx_text_c(&font_small, x + w / 2, y + h / 2 + f->line_h / 2, t, gfx_mix(0xFFFFFF, s->color, 90));
    }
}

static void avatar(const kstream_t *s, const char *path, int x, int y, int d, int ring) {
    const kimg_t *im = thumb_get(path);
    if (ring) { gfx_circle(x + d / 2, y + d / 2, d / 2 + 3, C_LIVE, 255); gfx_circle(x + d / 2, y + d / 2, d / 2 + 1, C_BG, 255); }
    if (im) gfx_blit_scaled_round(im->px, im->w, im->h, x, y, d, d, d/2);
    else {
        gfx_circle(x + d / 2, y + d / 2, d / 2, s->color, 255);
        const Font *f = d >= 56 ? &font_title : (d >= 36 ? &font_bold : &font_small);
        gfx_text_c(f, x + d / 2, y + (d - f->line_h) / 2, s->shortn, 0xFFFFFF);
    }
}

/* efecto hover de Twitch: la miniatura sube a la derecha y deja ver un bloque violeta */
static void hover_slab(int x, int y, int w, int h, int o) {
    gfx_rect(x, y, w, h, C_ACCENT);
    gfx_tri(x, y, x + o, y - o, x + o, y, C_ACCENT);
    gfx_tri(x + w, y + h, x + w + o, y + h - o, x + w, y + h - o, C_ACCENT);
    gfx_rect(x + w, y - o, o, h, C_ACCENT);
    gfx_rect(x + o, y - o, w - o, o, C_ACCENT);
}

/* ---------------------------------------------------------------- barra superior + lateral */
enum { TB_NONE = 0, TB_HOME, TB_BROWSE, TB_SEARCH, TB_SETTINGS, TB_ACCOUNT };
static int nav_tab(int x, const char *t, int on) {
    int w = gfx_text_w(&font_bold, t) + 2;
    int hov = ui_hover(x - 6, 4, w + 12, TOP_H - 8);
    text_bold(&font_bold, x, 13, t, (on || hov) ? C_LINK : C_TEXT);
    if (on) gfx_rect(x, TOP_H - 3, w, 3, C_ACCENT);
    return ui_click(x - 6, 4, w + 12, TOP_H - 8);
}
static int topbar(void) {
    int act = TB_NONE;
    gfx_rect(0, 0, SCR_W, TOP_H, C_BG2);
    gfx_rect(0, TOP_H - 1, SCR_W, 1, 0x000000);
    ui_logo(12, 8, 26);
    if (ui_click(6, 2, 40, 40)) act = TB_HOME;
    int home_on = screen == SCR_HOME && !cur_game[0];
    int browse_on = screen == SCR_BROWSE || (screen == SCR_HOME && cur_game[0]);
    if (nav_tab(52, "Inicio", home_on)) act = TB_HOME;
    if (nav_tab(52 + gfx_text_w(&font_bold, "Inicio") + 22, "Explorar", browse_on)) act = TB_BROWSE;
    /* Right: Iniciar sesion (always on-screen, pointer + A) then menu. */
    {
        int menu_w = 38, menu_x = SCR_W - 10 - menu_w;
        const char *login_lab = auth_ready() ? auth_login() : (auth_has_session() ? (auth_login()[0] ? auth_login() : "Cuenta") : "Iniciar sesi\xc3\xb3n");
        int login_style = auth_ready() ? BS_NORMAL : BS_ACCENT;
        int isz = 30 * 9 / 20;
        int login_w = gfx_text_w(&font_bold, login_lab) + isz + 8 + 16;
        int show_status, status_x, status_w, sx, sw, sy, sh, login_x, hov;
        u32 dc;
        const char *dl;
        if (login_w < 130) login_w = 130;
        if (login_w > 200) login_w = 200;
        login_x = menu_x - 8 - login_w;
        dc = !k_server[0] ? C_WARN : (k_server_ok ? (k_last_mock ? C_WARN : C_OKC) : C_LIVE);
        dl = !k_server[0] ? "Directo" : (k_server_ok ? (k_last_mock ? "DEMO" : "LAN") : "Sin servidor");
        status_w = 14 + gfx_text_w(&font_small, dl);
        sx = 228; sy = 7; sh = 30; show_status = 1;
        status_x = login_x - 8 - status_w;
        sw = status_x - 8 - sx;
        if (sw < 120) { show_status = 0; sw = login_x - 8 - sx; }
        if (sw < 80) sw = 80;
        hov = ui_hover(sx, sy, sw, sh);
        gfx_rrect(sx, sy, sw, sh, 6, C_BG, 255);
        gfx_rframe(sx, sy, sw - 34, sh, 6, hov ? 2 : 1, hov ? C_ACCENT : C_LINE, 255);
        gfx_text_fit(&font_small, sx + 10, sy + 7, search_q[0] ? search_q : "Buscar", search_q[0] ? C_TEXT : C_FAINT, sw - 60);
        gfx_rrect(sx + sw - 34, sy, 34, sh, 6, C_PANEL2, 255);
        gfx_icon(IC_SEARCH, sx + sw - 17, sy + sh / 2, 14, C_TEXT);
        if (ui_click(sx, sy, sw, sh)) act = TB_SEARCH;
        if (show_status) {
            gfx_circle(status_x + 4, 22, 4, dc, 255);
            gfx_text(&font_small, status_x + 13, 14, dl, C_DIM);
        }
        if (ui_button(login_x, 7, login_w, 30, login_lab, IC_USER, login_style, 0)) act = TB_ACCOUNT;
        if (ui_button(menu_x, 7, menu_w, 30, NULL, IC_MENU, BS_NORMAL, 0)) act = TB_SETTINGS;
    }
    return act;
}

static const char *sb_pick = NULL;
static void sidebar(void) {
    int i, y = TOP_H + 10, shown = 0;
    const klist_t *L = side.n ? &side : &live;
    gfx_rect(0, TOP_H, SB_W, SCR_H - TOP_H, C_PANEL);
    text_bold(&font_small, 10, y, "CANALES", C_TEXT);
    text_bold(&font_small, 10, y + 14, "RECOMENDADOS", C_TEXT);
    y += 36;
    for (i = 0; i < L->n && shown < 12 && y + 32 < SCR_H - 22; i++) {
        const kstream_t *s = &L->v[i];
        if (hidden_by_filter(s)) continue;
        int hov = ui_hover(2, y, SB_W - 4, 32);
        if (hov) gfx_rect(2, y, SB_W - 4, 32, 0x26262C);
        avatar(s, s->avatar, 8, y + 4, 24, 0);
        char v[24]; fmt_count(s->viewers, v, sizeof(v));
        int vw = gfx_text_w(&font_small, v);
        gfx_text_fit(&font_small, 38, y + 1, s->name, C_TEXT, SB_W - 50 - vw - 10);
        gfx_text_fit(&font_small, 38, y + 15, s->cat, C_DIM, SB_W - 46);
        gfx_circle(SB_W - 12 - vw, y + 9, 3, C_LIVE, 255);
        gfx_text(&font_small, SB_W - 6 - vw, y + 1, v, C_DIM);
        if (ui_click(2, y, SB_W - 4, 32)) sb_pick = s->slug;
        y += 33; shown++;
    }
    if (!shown) gfx_text(&font_small, 10, y, live_loaded ? "(sin canales)" : "Cargando...", C_FAINT);
    gfx_text(&font_small, 10, SCR_H - 20, "No oficial \xc2\xb7 v" TV_VERSION, C_FAINT);
}

/* ---------------------------------------------------------------- inicio: grilla */
static int card_pos(int i, int *x, int *y) {
    if (i >= nflt) { *x = MX; *y = GRID_Y + ((nflt + COLS - 1) / COLS) * (CH + CGAP_Y) - scroll; return *y + MORE_H >= VIEW_Y && *y < SCR_H; }
    *x = MX + (i % COLS) * (CW + CGAP_X);
    *y = GRID_Y + (i / COLS) * (CH + CGAP_Y) - scroll;
    return *y + CH >= VIEW_Y - 8 && *y < SCR_H;
}

static int draw_card(const kstream_t *s, int x, int y, int focus) {
    int hov = ui_hover(x, y - 6, CW + 6, CH + 6);
    int lift = hov || focus;
    int tx = x, ty = y;
    if (lift) { hover_slab(x, y, CW, TH, 6); tx += 6; ty -= 6; }
    const kimg_t *im = thumb_get(s->thumb);
    if (im) gfx_blit_scaled(im->px, im->w, im->h, tx, ty, CW, TH);
    else placeholder(s, tx, ty, CW, TH, &font_big);
    live_badge(tx + 8, ty + 8);
    viewers_pill(tx + 8, ty + TH - 26, s->viewers, " espectadores");
    if (s->mature) { gfx_rrect(tx + CW - 38, ty + 8, 30, 18, 3, 0x000000, 190); text_bold(&font_small, tx + CW - 33, ty + 9, "+18", 0xFFFFFF); }
    int my = y + TH + 8;
    avatar(s, s->avatar, x, my, 30, 0);
    gfx_text_fit(&font_bold, x + 38, my - 3, s->title[0] ? s->title : "(sin t\xc3\xadtulo)", lift ? C_LINK : C_TEXT, CW - 40);
    gfx_text_fit(&font_small, x + 38, my + 15, s->name, C_DIM, CW - 40);
    gfx_text_fit(&font_small, x + 38, my + 30, s->cat, C_DIM, CW - 40);
    int tx2 = x + 38, k;
    for (k = 0; k < s->ntags; k++) {
        int w = tag_pill(tx2, my + 48, s->tags[k], x + CW - tx2);
        if (!w) break;
        tx2 += w + 4;
        if (tx2 > x + CW - 30) break;
    }
    return ui_click(x, y - 6, CW + 6, CH + 6);
}

static int draw_more(int x, int y, int focus) {
    int w = MW, hov = ui_hover(x, y, w, MORE_H);
    const char *t = "Mostrar m\xc3\xa1s";
    int tw = gfx_text_w(&font_bold, t) + 26;
    gfx_rect(x, y + MORE_H / 2, (w - tw) / 2 - 10, 1, C_LINE);
    gfx_rect(x + (w + tw) / 2 + 10, y + MORE_H / 2, (w - tw) / 2 - 10, 1, C_LINE);
    if (hov || focus) gfx_rrect(x + (w - tw) / 2 - 8, y + 4, tw + 16, MORE_H - 8, 6, C_PANEL2, 255);
    text_bold(&font_bold, x + (w - tw) / 2, y + 8, t, C_LINK);
    gfx_icon(IC_DOWN, x + (w + tw) / 2 - 8, y + MORE_H / 2, 10, C_LINK);
    return ui_click(x, y, w, MORE_H);
}

enum { HA_NONE = 0, HA_OPEN, HA_MORE, HA_LANG, HA_REFRESH, HA_CAT, HA_BACKCAT, HA_OPENCAT, HA_ALLCATS };
static int h_act = HA_NONE, h_arg = 0;

static int grid_content(void) {
    int rows = (nflt + COLS - 1) / COLS;
    return grid_y_extra + rows * (CH + CGAP_Y) + (live_more ? MORE_H + 6 : 0);
}
static void ensure_visible(void) {
    int top, bot;
    if (sel >= nflt) { top = grid_y_extra + ((nflt + COLS - 1) / COLS) * (CH + CGAP_Y); bot = top + MORE_H; }
    else {
        top = grid_y_extra + (sel / COLS) * (CH + CGAP_Y);
        bot = top + CH;
        if (sel < COLS) top = 0; /* first row: bring Top categorias back into view */
    }
    if (top - 8 < scroll_t) scroll_t = top - 8;
    if (bot > scroll_t + VIEW_H - 8) scroll_t = bot - VIEW_H + 8;
}

/* Two lines under a shelf cover. Ellipsis only if the name does not fit in 2 lines. */
static void shelf_name(int x, int y, int maxw, const char *name, u32 col) {
    const Font *f = &font_small;
    int lh = f->line_h, h = lh * 2 + 3;
    char buf[160];
    const char *p, *q, *cut, *last_sp;
    int len;
    if (maxw < 8) return;
    gfx_rrect(x - 2, y - 1, maxw + 3, h, 3, C_BG, 230);
    if (gfx_text_w(f, name) <= maxw) { gfx_text(f, x, y, name, col); return; }
    /* longest prefix that fits on line 1; break at the last space when possible */
    cut = name; last_sp = NULL; p = name;
    while (*p) {
        q = p;
        utf8_next(&q);
        len = (int)(q - name);
        if (len >= (int)sizeof(buf)) break;
        memcpy(buf, name, (size_t)len); buf[len] = 0;
        if (gfx_text_w(f, buf) > maxw) break;
        if (*p == ' ') last_sp = p;
        cut = q; p = q;
    }
    if (last_sp && last_sp > name) cut = last_sp;
    len = (int)(cut - name);
    if (len <= 0) return;
    memcpy(buf, name, (size_t)len); buf[len] = 0;
    while (len > 0 && buf[len - 1] == ' ') buf[--len] = 0;
    gfx_text(f, x, y, buf, col);
    p = cut;
    while (*p == ' ') p++;
    if (*p) gfx_text_fit(f, x, y + lh, p, col, maxw);
}

static void draw_home_shelf(void) {
    int i, maxn, tw, x, y = GRID_Y_BASE - scroll, cw = 72, ch = 96, isz, lab_h, hit_h;
    u32 save;
    if (!nhome_cats) return;
    if (y + SHELF_H < VIEW_Y - 8 || y >= SCR_H) return;
    save = UI.down;
    if (in_ptr.y < VIEW_Y - 8) UI.down &= ~WPAD_BUTTON_A;
    text_bold(&font_bold, MX, y, "Top categor\xc3\xad" "as", C_LINK);
    /* Width must use font_bold + icon, same as ui_button, so the label is not clipped. */
    isz = 22 * 9 / 20;
    tw = gfx_text_w(&font_bold, "Ver todas") + isz + 8 + 16;
    if (ui_button(MX + MW - tw, y - 2, tw, 22, "Ver todas", IC_NEXT, BS_NORMAL, 0)) h_act = HA_ALLCATS;
    y += 24;
    maxn = gfx_widescreen() ? 7 : 5;
    if (maxn > nhome_cats) maxn = nhome_cats;
    lab_h = font_small.line_h * 2 + 4;
    hit_h = ch + 4 + lab_h;
    for (i = 0; i < maxn; i++) {
        const tcat_t *c = &home_cats[i];
        const kimg_t *im;
        int step = MW / maxn;
        x = MX + i * step;
        int hov = ui_hover(x, y, step - 4, hit_h);
        im = thumb_get(c->boxart);
        if (im) gfx_blit_scaled(im->px, im->w, im->h, x, y, cw, ch);
        else gfx_vgrad(x, y, cw, ch, c->color, gfx_mix(c->color, 0, 160));
        shelf_name(x, y + ch + 3, step - 6, c->name, hov ? C_LINK : C_TEXT);
        if (ui_click(x, y, step - 4, hit_h)) { h_act = HA_OPENCAT; h_arg = i; }
    }
    UI.down = save;
}

static void draw_home(void) {
    int i;
    gfx_clear(C_BG);
    grid_y_extra = (screen == SCR_HOME && !cur_game[0] && nhome_cats) ? SHELF_H : 0;
    if (cur_game[0]) {
        const kimg_t *bx = thumb_get(cur_game_box);
        if (bx) gfx_blit_scaled(bx->px, bx->w, bx->h, MX, HDR_Y - 2, 36, 48);
        else { gfx_rect(MX, HDR_Y - 2, 36, 48, C_ACCENT2); gfx_text_c(&font_bold, MX + 18, HDR_Y + 13, "?", 0xFFFFFF); }
        text_bold(&font_title, MX + 46, HDR_Y - 4, cur_game_name, C_TEXT);
        char v[24], l[80]; fmt_count(cur_game_viewers, v, sizeof(v));
        if (cur_game_viewers > 0) snprintf(l, sizeof(l), "%s espectadores  \xc2\xb7  Idioma: %s", v, k_lang_names[k_lang]);
        else snprintf(l, sizeof(l), "Idioma: %s", k_lang_names[k_lang]);
        gfx_text_fit(&font_small, MX + 46, HDR_Y + 24, l, C_DIM, MW - 150);
        if (ui_click(MX + 46, HDR_Y + 22, MW - 150, 20)) h_act = HA_LANG;
        if (ui_button(MX + MW - 100, HDR_Y + 2, 100, 26, "Explorar", IC_BACK, BS_NORMAL, 0)) h_act = HA_BACKCAT;
    } else {
        const char *a = "Canales en vivo", *b = " que creemos que te gustar\xc3\xa1n";
        text_bold(&font_bold, MX, HDR_Y, a, C_LINK);
        text_bold(&font_bold, MX + gfx_text_w(&font_bold, a) + 1, HDR_Y, b, C_TEXT);
        int cx = MX;
        char lab[40]; snprintf(lab, sizeof(lab), "Idioma: %s", k_lang_names[k_lang]);
        int lw = chip(cx, CHIP_Y, lab, 0, 0);
        if (ui_click(cx, CHIP_Y, lw, 22)) h_act = HA_LANG;
        cx += lw + 8;
        gfx_rect(cx - 4, CHIP_Y + 3, 1, 16, C_LINE);
        for (i = 0; i <= ncats; i++) {
            const char *t = i ? cats[i - 1] : "Todo";
            int w = gfx_text_w(&font_small, t) + 18;
            if (cx + w > MX + MW - 34) break;
            chip(cx, CHIP_Y, t, cat_sel == i, 0);
            if (ui_click(cx, CHIP_Y, w, 22)) { h_act = HA_CAT; h_arg = i; }
            cx += w + 6;
        }
        if (ui_button(MX + MW - 28, CHIP_Y - 1, 28, 24, NULL, IC_NEXT, BS_NORMAL, 0)) h_act = HA_REFRESH;
    }
    int ncards = nflt + (live_more ? 1 : 0);
    int content = grid_content();
    int maxs = content > VIEW_H - 8 ? content - VIEW_H + 8 : 0;
    if (scroll_t > maxs) scroll_t = maxs;
    if (scroll_t < 0) scroll_t = 0;
    scroll += (scroll_t - scroll) / 3;
    if (abs(scroll_t - scroll) < 3) scroll = scroll_t;
    gfx_clip(SB_W, VIEW_Y - 8, SCR_W - SB_W, VIEW_H + 8);
    if (!cur_game[0] && nhome_cats) draw_home_shelf();
    for (i = 0; i < ncards; i++) {
        int x, y;
        if (!card_pos(i, &x, &y)) continue;
        u32 save = UI.down;
        if (in_ptr.y < VIEW_Y - 8) UI.down &= ~WPAD_BUTTON_A;
        int foc = i == sel && !in_ptr.valid;
        if (i < nflt) { const kstream_t *card = filtered_stream(i); if (card && draw_card(card, x, y, foc)) { h_act = HA_OPEN; h_arg = i; } }
        else if (draw_more(x, y, foc)) h_act = HA_MORE;
        UI.down = save;
    }
    gfx_noclip();
    if (!nflt) {
        const char *a = live_loaded ? (live_err[0] ? "No se pudo cargar la lista" : "No hay directos para este filtro") : "Cargando...";
        gfx_text_c(&font_bold, MX + MW / 2, GRID_Y + 110, a, C_DIM);
        if (live_err[0]) {
            char **ln; int n = ui_wrap(&font_small, live_err, MW - 40, &ln), k;
            for (k = 0; k < n && k < 4; k++) gfx_text_c(&font_small, MX + MW / 2, GRID_Y + 140 + k * 16, ln[k], C_FAINT);
            for (k = 0; k < n; k++) free(ln[k]);
            free(ln);
        }
        if (nhidden) gfx_text_c(&font_small, MX + MW / 2, GRID_Y + 210, "(hay canales ocultos por el filtro +18/apuestas)", C_FAINT);
    }
    if (maxs > 0) {
        int th = VIEW_H * VIEW_H / (content + 1), ty = VIEW_Y + (VIEW_H - th) * scroll / maxs;
        (void)th; (void)ty;   /* rc10: sin barra de desplazamiento al costado de la grilla */
        if (ui_hover(MX, SCR_H - 22, MW, 22) && scroll_t < maxs) scroll_t += 6;
        if (ui_hover(MX, VIEW_Y - 4, MW, 14) && scroll_t > 0) scroll_t -= 6;
    }
    sidebar();
    int tb = topbar();
    if (tb) h_act = 100 + tb;
    if (k_last_mock && k_server_ok) {
        gfx_rrect(MX + 20, SCR_H - 30, MW - 40, 24, 6, C_WARN, 240);
        gfx_text_fit(&font_small, MX + 30, SCR_H - 26, k_notice[0] ? k_notice : "DATOS DEMO (Twitch no respondi\xc3\xb3)", C_DARK, MW - 60);
    }
    ui_draw_toast(SCR_H - 34);
}

/* ---------------------------------------------------------------- explorar (categorias) */
static int b_act = 0, b_arg = 0;
#define BGRID_Y (TOP_H + 92)
#define BGRID_H (SCR_H - BGRID_Y)
static void draw_browse(void) {
    int i;
    gfx_clear(C_BG);
    text_bold(&font_big, MX, TOP_H + 6, "Explorar", C_TEXT);
    int tx = MX, ty = TOP_H + 52;
    text_bold(&font_bold, tx, ty, "Categor\xc3\xad" "as", C_LINK);
    int w1 = gfx_text_w(&font_bold, "Categor\xc3\xad" "as");
    gfx_rect(tx, ty + 22, w1, 3, C_ACCENT);
    int tx2 = tx + w1 + 26, w2 = gfx_text_w(&font_bold, "Canales en vivo");
    int hov = ui_hover(tx2 - 4, ty - 4, w2 + 8, 28);
    text_bold(&font_bold, tx2, ty, "Canales en vivo", hov ? C_LINK : C_TEXT);
    if (ui_click(tx2 - 4, ty - 4, w2 + 8, 28)) b_act = 2;
    if (ui_button(MX + MW - 118, TOP_H + 8, 118, 26, cat_search_q[0] ? "Todas" : "Buscar", IC_SEARCH, BS_NORMAL, 0))
        b_act = cat_search_q[0] ? 4 : 5;
    if (cat_search_q[0]) {
        char qlab[80];
        snprintf(qlab, sizeof(qlab), "Busqueda: %s (%d)", cat_search_q, bcats.n);
        gfx_text_fit(&font_small, MX + w1 + w2 + 40, ty + 4, qlab, C_DIM, MW - w1 - w2 - 50);
    } else {
        char qlab[64];
        snprintf(qlab, sizeof(qlab), "%d categor\xc3\xad" "as%s", bcats.n, bcats_more ? "  \xc2\xb7  scroll para mas" : "");
        gfx_text_fit(&font_small, MX + w1 + w2 + 40, ty + 4, qlab, C_DIM, MW - w1 - w2 - 50);
    }
    gfx_rect(MX, ty + 25, MW, 1, C_LINE);
    int extra = (bcats_more && !cat_search_q[0]) ? 1 : 0;
    int nshow = bcats.n + extra;
    int rows = (nshow + BCOLS - 1) / BCOLS, content = rows * BCH;
    int maxs = content > BGRID_H - 8 ? content - BGRID_H + 8 : 0;
    if (!in_ptr.valid) {
        int top = (bsel / BCOLS) * BCH, bot = top + BCH;
        if (top < bscroll_t) bscroll_t = top;
        if (bot > bscroll_t + BGRID_H - 8) bscroll_t = bot - BGRID_H + 8;
    }
    if (bscroll_t > maxs) bscroll_t = maxs;
    if (bscroll_t < 0) bscroll_t = 0;
    bscroll += (bscroll_t - bscroll) / 3;
    if (abs(bscroll_t - bscroll) < 3) bscroll = bscroll_t;
    gfx_clip(SB_W, BGRID_Y - 8, SCR_W - SB_W, BGRID_H + 8);
    for (i = 0; i < nshow; i++) {
        int x = MX + (i % BCOLS) * (BW + BGAP), y = BGRID_Y + (i / BCOLS) * BCH - bscroll;
        if (y + BCH < BGRID_Y - 8 || y > SCR_H) continue;
        u32 save = UI.down;
        if (in_ptr.y < BGRID_Y - 8) UI.down &= ~WPAD_BUTTON_A;
        int h = ui_hover(x, y - 6, BW + 6, BCH), foc = (i == bsel && !in_ptr.valid) || h;
        if (i >= bcats.n) {
            const char *t = bcats_loading ? "Cargando..." : "Mostrar m\xc3\xa1s";
            gfx_rrect(x, y, BW, BH, 6, C_PANEL2, 255);
            gfx_text_fit(&font_bold, x + 6, y + BH / 2 - 8, t, C_LINK, BW - 12);
            if (ui_click(x, y - 6, BW + 6, BCH)) b_act = 3;
            if (h) bsel = i;
            UI.down = save;
            continue;
        }
        const tcat_t *c = &bcats.v[i];
        int bx = x, by = y;
        if (foc) { hover_slab(x, y, BW, BH, 6); bx += 6; by -= 6; }
        const kimg_t *im = thumb_get(c->boxart);
        if (im) gfx_blit(im->px, im->w, im->h, bx, by);
        else {
            gfx_vgrad(bx, by, BW, BH, c->color, gfx_mix(c->color, 0, 160));
            char **ln; int n = ui_wrap(&font_bold, c->name, BW - 10, &ln), k;
            for (k = 0; k < n && k < 4; k++) gfx_text_c(&font_bold, bx + BW / 2, by + 20 + k * 18, ln[k], 0xFFFFFF);
            for (k = 0; k < n; k++) free(ln[k]);
            free(ln);
        }
        gfx_text_fit(&font_bold, x, y + BH + 4, c->name, foc ? C_LINK : C_TEXT, BW + 20);
        if (c->viewers > 0) {   /* rc11: sin dato (o 0) no se muestra "0 espectadores" */
            char v[24], l[48]; fmt_count(c->viewers, v, sizeof(v)); snprintf(l, sizeof(l), "%s espectadores", v);
            gfx_text_fit(&font_small, x, y + BH + 22, l, C_DIM, BW + 20);
        }
        if (ui_click(x, y - 6, BW + 6, BCH)) { b_act = 1; b_arg = i; }
        if (h) bsel = i;
        UI.down = save;
    }
    gfx_noclip();
    if (!bcats.n) gfx_text_c(&font_bold, MX + MW / 2, BGRID_Y + 100, bloaded ? (berr[0] ? berr : "Sin categor\xc3\xad" "as") : "Cargando...", C_DIM);
    if (maxs > 0) {
        int th = BGRID_H * BGRID_H / (content + 1), ty2 = BGRID_Y + (BGRID_H - th) * bscroll / maxs;
        (void)th; (void)ty2;   /* rc10: sin barra de desplazamiento */
        if (ui_hover(MX, SCR_H - 22, MW, 22) && bscroll_t < maxs) bscroll_t += 6;
        if (ui_hover(MX, BGRID_Y - 4, MW, 14) && bscroll_t > 0) bscroll_t -= 6;
    }
    /* Auto-load the next page when the last rows come into view. */
    if (bcats_more && !bcats_loading && !cat_search_q[0] && bcats.n) {
        int lasty = BGRID_Y + ((bcats.n - 1) / BCOLS) * BCH - bscroll;
        if (lasty < SCR_H - 20 || bsel >= bcats.n - 2 * BCOLS) b_act = 3;
    }
    sidebar();
    int tb = topbar();
    if (tb) b_act = 100 + tb;
    ui_draw_toast(SCR_H - 30);
}

/* ---------------------------------------------------------------- busqueda */
static int s_act = 0, s_arg = 0;
static void draw_search(void) {
    int i, y0 = TOP_H + 72, rh = 54;
    gfx_clear(C_BG);
    char t[96]; snprintf(t, sizeof(t), "Resultados de b\xc3\xbasqueda para \"%s\"", search_q);
    text_bold(&font_bold, MX, TOP_H + 12, t, C_TEXT);
    if (search_cats.n) {
        int k, maxn = gfx_widescreen() ? 6 : 4, cw = 54, ch = 72, yy = TOP_H + 38;
        text_bold(&font_small, MX, yy, "CategorÃ­" "as", C_LINK);
        yy += 16;
        if (maxn > search_cats.n) maxn = search_cats.n;
        for (k = 0; k < maxn; k++) {
            const tcat_t *c = &search_cats.v[k];
            int xx = MX + k * (MW / maxn);
            const kimg_t *im = thumb_get(c->boxart);
            int chov = ui_hover(xx, yy, cw + 8, ch + 18);
            if (im) gfx_blit_scaled(im->px, im->w, im->h, xx, yy, cw, ch);
            else gfx_vgrad(xx, yy, cw, ch, c->color, gfx_mix(c->color, 0, 160));
            gfx_text_fit(&font_small, xx, yy + ch + 1, c->name, chov ? C_LINK : C_TEXT, MW / maxn - 4);
            if (ui_click(xx, yy, cw + 8, ch + 18)) { s_act = 3; s_arg = k; }
        }
        y0 = yy + ch + 24;
    }
    text_bold(&font_bold, MX, y0 - 22, "Canales", C_TEXT);
    gfx_rect(MX, y0 - 4, MW, 1, C_LINE);
    int extra = valid_login(search_q) ? 1 : 0;
    int n = results.n + extra, vis = (SCR_H - y0 - 26) / rh;
    if (rsel >= n) rsel = n ? n - 1 : 0;
    if (rsel < rscroll) rscroll = rsel;
    if (rsel >= rscroll + vis) rscroll = rsel - vis + 1;
    for (i = 0; i < vis && rscroll + i < n; i++) {
        int k = rscroll + i, y = y0 + i * rh;
        int foc = k == rsel && !in_ptr.valid, hov = ui_hover(MX, y, MW, rh - 4);
        if (hov || foc) gfx_rrect(MX, y, MW, rh - 4, 6, C_PANEL, 255);
        if (foc) gfx_rframe(MX, y, MW, rh - 4, 6, 2, C_ACCENT, 255);
        if (extra && k == 0) {
            gfx_circle(MX + 30, y + 25, 20, C_ACCENT, 255);
            gfx_icon(IC_PLAY, MX + 32, y + 25, 16, 0xFFFFFF);
            char g[96]; snprintf(g, sizeof(g), "Ir a twitch.tv/%s", search_q);
            text_bold(&font_bold, MX + 62, y + 15, g, C_TEXT);
        } else {
            const kstream_t *s = &results.v[k - extra];
            avatar(s, s->avatar, MX + 10, y + 5, 40, s->live > 0);
            gfx_text_fit(&font_bold, MX + 62, y + 4, s->name, hov || foc ? C_LINK : C_TEXT, MW - 180);
            int nw = gfx_text_w(&font_bold, s->name);
            if (s->verified && nw < MW - 180) partner_check(MX + 70 + nw, y + 13, 7);
            char f[96], v[24];
            if (s->live > 0) {
                int bw = live_badge(MX + 62, y + 25);
                fmt_count(s->viewers, v, sizeof(v));
                snprintf(f, sizeof(f), "%s%s%s espectadores", s->cat, s->cat[0] ? "  \xc2\xb7  " : "", v);
                gfx_text_fit(&font_small, MX + 70 + bw, y + 26, f, C_DIM, MW - 90 - bw);
            } else {
                fmt_count(s->followers, v, sizeof(v)); snprintf(f, sizeof(f), "%s seguidores", v);
                gfx_text(&font_small, MX + 62, y + 26, f, C_DIM);
            }
        }
        if (ui_click(MX, y, MW, rh - 4)) { s_act = 1; s_arg = k; }
        if (hov) rsel = k;
    }
    if (!n) gfx_text_c(&font_bold, MX + MW / 2, y0 + 80, search_err[0] ? search_err : "Sin resultados", C_DIM);
    gfx_text(&font_small, MX, SCR_H - 20, "A: abrir canal   B: volver   1: nueva b\xc3\xbasqueda", C_FAINT);
    sidebar();
    int tb = topbar();
    if (tb) s_act = 100 + tb;
    ui_draw_toast(SCR_H - 30);
}

/* ---------------------------------------------------------------- pagina de canal */
static int c_act = 0, c_arg = 0;
#define PV_X 12
#define PV_Y (TOP_H + 10)
#define PV_W 384
#define PV_H 216
#define RX   (PV_X + PV_W + 12)
#define RW   (SCR_W - RX - 12)
#define VX   12
#define VY   (PV_Y + PV_H + 96)
#define VTW  140
#define VTH  79
static void draw_channel(void) {
    const kstream_t *s = &chan;
    char v[24], line[200];
    int k;
    gfx_clear(C_BG);
    const kimg_t *th = thumb_get(s->thumb);
    if (th) gfx_blit_scaled(th->px, th->w, th->h, PV_X, PV_Y, PV_W, PV_H);
    else placeholder(s, PV_X, PV_Y, PV_W, PV_H, &font_big);
    if (s->live > 0) {
        int hov = ui_hover(PV_X, PV_Y, PV_W, PV_H);
        gfx_rect_a(PV_X, PV_Y, PV_W, PV_H, 0x000000, hov ? 30 : 80);
        gfx_circle(PV_X + PV_W / 2, PV_Y + PV_H / 2, 32, hov ? C_ACCENT_HI : C_ACCENT, 255);
        gfx_icon(IC_PLAY, PV_X + PV_W / 2 + 3, PV_Y + PV_H / 2, 26, 0xFFFFFF);
        live_badge(PV_X + 10, PV_Y + 10);
        viewers_pill(PV_X + 10, PV_Y + PV_H - 28, s->viewers, " espectadores");
        if (ui_click(PV_X, PV_Y, PV_W, PV_H)) c_act = 1;
    } else {
        gfx_rect_a(PV_X, PV_Y, PV_W, PV_H, 0x000000, 150);
        offline_badge(PV_X + 10, PV_Y + 10);
        gfx_text_c(&font_bold, PV_X + PV_W / 2, PV_Y + PV_H / 2 - 9, chan_err[0] ? "No se pudo cargar el canal" : "Desconectado", C_DIM);
    }
    /* columna derecha */
    int y = PV_Y;
    gfx_rrect(RX, y, RW, PV_H, 6, C_PANEL, 255);
    y += 10;
    if (s->live > 0) {
        char up[24]; fmt_uptime(s->start, up, sizeof(up));
        fmt_count(s->viewers, v, sizeof(v));
        gfx_circle(RX + 16, y + 8, 4, C_LIVE, 255);
        text_bold(&font_bold, RX + 26, y, v, C_LIVE);
        if (up[0]) gfx_text(&font_small, RX + RW - 12 - gfx_text_w(&font_small, up), y + 2, up, C_DIM);
    } else gfx_text(&font_small, RX + 12, y + 2, "Desconectado", C_DIM);
    y += 22;
    fmt_count(s->followers, v, sizeof(v));
    snprintf(line, sizeof(line), "%s seguidores%s", v, s->mock ? " [DEMO]" : "");
    gfx_text_fit(&font_small, RX + 12, y, line, C_DIM, RW - 20);
    y += 22;
    int bw=RW-20,bx=RX+10;y=PV_Y+50;
    if(s->live>0&&ui_button(bx,y,bw,22,"Ver en vivo",IC_PLAY,BS_ACCENT,chan_focus==0&&!in_ptr.valid))c_act=1;
    y+=26;if(ui_button(bx,y,bw,22,social_label(s->slug),IC_HEART,BS_ACCENT,chan_focus==1&&!in_ptr.valid))c_act=6;
    y+=26;if(ui_button(bx,y,bw,22,"Suscribirse",0,BS_NORMAL,chan_focus==2&&!in_ptr.valid))c_act=7;
    y+=26;snprintf(line,sizeof(line),"Chat: %s",k_chat_on?"Si":"No");if(ui_button(bx,y,bw,22,line,0,BS_NORMAL,chan_focus==3&&!in_ptr.valid))c_act=2;
    y+=26;if(ui_button(bx,y,bw,22,k_server[0]?pm_names[k_play_mode]:"Directo",0,BS_NORMAL,chan_focus==4&&!in_ptr.valid))c_act=3;
    y+=26;if(ui_button(bx,y,bw,22,"Volver",IC_BACK,BS_NORMAL,chan_focus==5&&!in_ptr.valid))c_act=4;
    /* Published profile banner, also in direct mode; absent banners remain a plain UI background. */
    const kimg_t *bn=thumb_get(s->banner);
    if(bn) {gfx_blit_scaled(bn->px,bn->w,bn->h,0,PV_Y+PV_H+4,SCR_W,76);gfx_rect_a(0,PV_Y+PV_H+4,SCR_W,76,0,155);}
    /* cabecera del canal debajo del reproductor (como en twitch.tv) */
    int hy = PV_Y + PV_H + 12;
    avatar(s, s->avatar, PV_X + 4, hy + 2, 52, s->live > 0);
    if (s->live > 0) live_badge(PV_X + 4 + 26 - (gfx_text_w(&font_small, "EN VIVO") + 12) / 2, hy + 41);
    int nx = PV_X + 70;
    text_bold(&font_title, nx, hy - 2, s->name, C_TEXT);
    int nw = gfx_text_w(&font_title, s->name) + 1;
    if (s->verified) partner_check(nx + nw + 12, hy + 12, 8);
    gfx_text_fit(&font_bold, nx, hy + 26, s->live > 0 ? (s->title[0] ? s->title : "(sin t\xc3\xadtulo)") : (s->bio[0] ? s->bio : (chan_err[0] ? chan_err : "El canal no est\xc3\xa1 en vivo.")), C_TEXT, SCR_W - nx - 16);
    int cx = nx;
    if (s->cat[0]) { int cw = gfx_text_w(&font_small, s->cat); if (cw > 200) cw = 200; gfx_text_fit(&font_small, cx, hy + 48, s->cat, C_LINK, 200); cx += cw + 10; }
    for (k = 0; k < s->ntags && cx < SCR_W - 60; k++) { int w = tag_pill(cx, hy + 47, s->tags[k], SCR_W - 16 - cx); if (!w) break; cx += w + 4; }
    if (s->mature && cx < SCR_W - 50) tag_pill(cx, hy + 47, "+18", 0);
    /* videos recientes */
    text_bold(&font_bold, VX, VY - 22, "Videos recientes", C_TEXT);
    if (!nvods) gfx_text(&font_small, VX + 150, VY - 21, "(sin videos con miniatura disponible)", C_FAINT);
    for (k = 0; k < nvods && k < 4; k++) {
        const tvod_t *vd = &vods[k];
        int x = VX + k * (VTW + 16), yy = VY;
        int foc = (chan_focus == 6 + k && !in_ptr.valid), hov = ui_hover(x, yy - 6, VTW + 6, VTH + 26);
        int ox = x, oy = yy;
        if (foc || hov) { hover_slab(x, yy, VTW, VTH, 5); ox += 5; oy -= 5; }
        const kimg_t *im = thumb_get(vd->thumb);
        if (im) gfx_blit_scaled(im->px, im->w, im->h, ox, oy, VTW, VTH);
        else { gfx_vgrad(ox, oy, VTW, VTH, 0x3A2A5A, 0x18181B); gfx_icon(IC_PLAY, ox + VTW / 2, oy + VTH / 2, 18, C_DIM); }
        char d[24]; fmt_dur(vd->length, d, sizeof(d));
        int dw = gfx_text_w(&font_small, d) + 8;
        gfx_rrect(ox + 4, oy + 4, dw, 17, 3, 0x000000, 180); gfx_text(&font_small, ox + 8, oy + 5, d, 0xFFFFFF);
        char ag[32]; fmt_ago(vd->created, ag, sizeof(ag));
        if (ag[0]) { int aw = gfx_text_w(&font_small, ag) + 8; gfx_rrect(ox + 4, oy + VTH - 21, aw, 17, 3, 0x000000, 180); gfx_text(&font_small, ox + 8, oy + VTH - 20, ag, 0xFFFFFF); }
        gfx_text_fit(&font_small, x, yy + VTH + 3, vd->title, foc || hov ? C_LINK : C_TEXT, VTW + 4);
        if (ui_click(x, yy - 6, VTW + 6, VTH + 26)) { c_act = 5; c_arg = k; }
    }
    int tb = topbar();
    if (tb) c_act = 100 + tb;
    ui_draw_toast(SCR_H - 30);
}

/* ---------------------------------------------------------------- viendo: chat a la derecha o pantalla completa */
static unsigned char rowsrc[SCR_H];
#define OSD_TOP 60
#define OSD_BOT (SCR_H - 52)
#define CHAT_X  (gfx_widescreen()?SCR_W-240:448)
#define CHAT_W  (SCR_W - CHAT_X)
static void draw_vu(int x, int y, int w, int h) {
    float l = tp_audio_level();
    int n = 16, i, bw = (w - (n - 1) * 2) / n;
    for (i = 0; i < n; i++) {
        int on = l * n > i;
        gfx_rrect(x + i * (bw + 2), y, bw, h, 2, on ? (i > 12 ? C_LIVE : C_ACCENT) : C_PANEL2, 255);
    }
}
static int chat_layout(void) { return watch_details && !watching_vod; }
static void watch_status(char *s, int sz) {
    if (tp_state() == TP_ERROR) snprintf(s, sz, "Se cort\xc3\xb3: %s", tp_error());
    else snprintf(s, sz, "%s \xc2\xb7 %s \xc2\xb7 %.0f fps \xc2\xb7 vol %d%%", tp_status(),
                  watching_vod ? (k_server[0]?"VOD transcodificado":"VOD directo") : (k_server[0] ? pm_names[k_play_mode] : tp_mode_text()), tp_fps(), tp_volume() * 100 / 255);
}
static void draw_chat_panel(void) {
    static tchat_t msgs[24];
    int n = chat_get(msgs, 24), i, k;
    chat_anim_begin();   /* rc8: la lista de emotes animados se arma de nuevo */
    gfx_rect(CHAT_X, 0, CHAT_W, SCR_H, C_BG2);
    gfx_rect(CHAT_X, 0, 1, SCR_H, 0x000000);
    text_bold(&font_small, CHAT_X + 10, 10, "CHAT DEL DIRECTO", C_TEXT);
    gfx_rect(CHAT_X, 34, CHAT_W, 1, C_LINE);
    int y = SCR_H - 106;
    for (i = n - 1; i >= 0 && y > 40; i--) {
        char buf[260];
        int nl_name = strlen(msgs[i].name);
        int rich = !msgs[i].sys && msgs[i].ne;   /* rc7: emotes como imagenes */
        if (msgs[i].sys) snprintf(buf, sizeof(buf), "%s", msgs[i].text);
        else snprintf(buf, sizeof(buf), "%s: %s", msgs[i].name, msgs[i].text);
        char **ln = NULL; int nl = 0, hgt;
        if (rich) hgt = chat_rich(&font_small, CHAT_X + 9, 0, CHAT_W - 18, &msgs[i], msgs[i].color, C_TEXT, 0) + 4;
        else { nl = ui_wrap(&font_small, buf, CHAT_W - 18, &ln); hgt = nl * 15 + 4; }
        if (y - hgt < 40) { for (k = 0; k < nl; k++) free(ln[k]); free(ln); break; }
        y -= hgt;
        if (rich) { chat_rich(&font_small, CHAT_X + 9, y, CHAT_W - 18, &msgs[i], msgs[i].color, C_TEXT, 1); continue; }
        for (k = 0; k < nl; k++) {
            int yy = y + k * 15;
            if (msgs[i].sys) gfx_text(&font_small, CHAT_X + 9, yy, ln[k], C_FAINT);
            else if (k == 0 && nl_name && !strncmp(ln[k], msgs[i].name, nl_name)) {
                /* nombre en su color (negrita) + el resto en blanco */
                text_bold(&font_small, CHAT_X + 9, yy, msgs[i].name, msgs[i].color);
                gfx_text(&font_small, CHAT_X + 10 + gfx_text_w(&font_small, msgs[i].name), yy, ln[k] + nl_name, C_TEXT);
            } else gfx_text(&font_small, CHAT_X + 9, yy, ln[k], C_TEXT);
        }
        for (k = 0; k < nl; k++) free(ln[k]);
        free(ln);
    }
    if (!n) {
        char **ln;int nn=ui_wrap(&font_small,chat_status()[0]?chat_status():"Esperando mensajes...",CHAT_W-20,&ln);
        for(i=0;i<nn;i++){gfx_text(&font_small,CHAT_X+10,190+i*16,ln[i],C_FAINT);free(ln[i]);}free(ln);
    }
    if(ui_button(CHAT_X+8,SCR_H-98,CHAT_W-16,26,social_label(pinfo.slug),IC_HEART,BS_ACCENT,0))watch_social_pressed=1;
    if(ui_button(CHAT_X+8,SCR_H-66,CHAT_W-16,26,"Suscribirse",0,BS_NORMAL,0))watch_social_pressed=2;
    if(ui_button(CHAT_X+8,SCR_H-34,CHAT_W-16,26,"Escribir",IC_CHAT,BS_ACCENT,0))chat_write_pressed=1;
}
static void draw_info_panel(int vh) {
    const kstream_t *c = &pinfo;
    char s[240], v[24];
    int y = vh + 10;
    gfx_rect(0, vh, CHAT_X, SCR_H - vh, C_BG);
    const kimg_t *bn=thumb_get(c->banner);
    if(bn){gfx_blit_scaled(bn->px,bn->w,bn->h,0,vh,CHAT_X,62);gfx_rect_a(0,vh,CHAT_X,62,0,175);}
    avatar(c, c->avatar, 12, y + 2, 40, tp_state() == TP_PLAYING && !watching_vod);
    text_bold(&font_bold, 62, y, c->name, C_TEXT);
    int nw = gfx_text_w(&font_bold, c->name) + 1;
    if (c->verified) partner_check(62 + nw + 10, y + 9, 6);
    gfx_text_fit(&font_small, 62, y + 20, c->title, C_TEXT, CHAT_X - 74);
    gfx_text_fit(&font_small, 62, y + 36, watching_vod ? pvod.cat : c->cat, C_LINK, 200);
    if (!watching_vod) {
        fmt_count(c->viewers, v, sizeof(v));
        char up[24]; fmt_uptime(c->start, up, sizeof(up));
        snprintf(s, sizeof(s), "%s%s%s", v, up[0] ? "  \xc2\xb7  " : "", up);
        int sw = gfx_text_w(&font_small, s);
        gfx_circle(CHAT_X - 20 - sw, y + 44, 3, C_LIVE, 255);
        gfx_text(&font_small, CHAT_X - 12 - sw, y + 36, s, C_LIVE);
    }
    watch_status(s, sizeof(s));
    /* Full Wii Remote legend for this live windowed player (only bindings that actually work). */
    const char *legend =
        "B: canal/videos   Izq/Der: volumen   +: pantalla completa   "
        "1: escribir   -: seguir   2: suscribirse";
    char **lln = NULL;
    int maxw = CHAT_X - 24, nleg = ui_wrap(&font_small, legend, maxw, &lln), li;
    int nwrap = nleg, nshow;
    if (nleg < 1) nleg = 1;
    nshow = nleg > 3 ? 3 : nleg;
    int legend_h = nshow * font_small.line_h + 6;
    int legend_y = SCR_H - legend_h;
    if (legend_y < vh + 8) { legend_y = vh + 8; legend_h = SCR_H - legend_y; }
    /* rc4: the status line (vh+62, one font_small line) must never touch the thumbnails.
       Thumbnails start at least STATUS_GAP px below the status line's bottom. */
    const int status_y = vh + 62, status_bot = status_y + font_small.line_h, STATUS_GAP = 6;
    int compact=vh>280,sy=compact?status_bot+STATUS_GAP:vh+112,sh=compact?32:54,k;
    int title_h = compact ? 0 : (font_small.line_h + 2);
    if (sy + sh + title_h > legend_y - 4) {
        sh = legend_y - 4 - title_h - sy;
        if (sh < 20) sh = 20;
    }
    gfx_text_fit(&font_small,12,status_y,s,C_DIM,CHAT_X-24);
    if(!compact) {draw_vu(12,vh+84,100,5);gfx_text(&font_small,128,vh+88,"Videos recientes",C_DIM);}
    for(k=0;k<nvods&&k<4;k++) {
        int x=12+k*108;const kimg_t *im=thumb_get(vods[k].thumb);
        if(im)gfx_blit_scaled_round(im->px,im->w,im->h,x,sy,100,sh,3);
        else gfx_rect(x,sy,100,sh,C_PANEL);
        if(!compact && sy + sh + title_h <= legend_y) gfx_text_fit(&font_small,x,sy+sh+3,vods[k].title,C_DIM,102);
    }
    gfx_rect(8, legend_y - 2, CHAT_X - 16, legend_h, C_BG);
    for (li = 0; li < nshow && lln; li++) gfx_text_fit(&font_small, 12, legend_y + li * font_small.line_h, lln[li], C_DIM, maxw);
    if (lln) { for (li = 0; li < nwrap; li++) free(lln[li]); free(lln); }
    ui_draw_toast(vh - 6);
}
static void draw_osd(void) {
    const kstream_t *c = &pinfo;
    char s[240], v[24];
    gfx_vgrad(0, 0, SCR_W, OSD_TOP, 0x000000, gfx_mix(0x000000, C_BG2, 120));
    gfx_rect(0, OSD_BOT, SCR_W, SCR_H - OSD_BOT, C_BG2);
    gfx_rect(0, OSD_BOT, SCR_W, 1, 0x000000);
    avatar(c, c->avatar, 14, 12, 36, tp_state() == TP_PLAYING && !watching_vod);
    text_bold(&font_bold, 62, 10, c->name, C_TEXT);
    int nw = gfx_text_w(&font_bold, c->name) + 1;
    if (tp_state() == TP_PLAYING && !watching_vod) live_badge(62 + nw + 10, 11);
    gfx_text_fit(&font_small, 62, 32, c->title, C_DIM, 450);
    if (!watching_vod) {
        fmt_count(c->viewers, v, sizeof(v));
        gfx_circle(SCR_W - 104, 19, 4, C_LIVE, 255);
        gfx_text(&font_small, SCR_W - 96, 11, v, C_TEXT);
    }
    ui_logo(SCR_W - 40, 26, 24);
    watch_status(s, sizeof(s));
    gfx_text_fit(&font_bold, 16, OSD_BOT + 8, s, C_TEXT, SCR_W-40);
    gfx_text_fit(&font_small, 16, OSD_BOT + 30, !watching_vod ? "B: volver   +: pantalla   1: escribir   -: seguir   2: suscribir" : "B: volver   Izq/Der: volumen   A: mostrar/ocultar", C_FAINT, SCR_W-310);
    if(ui_button(SCR_W-280,OSD_BOT+26,132,24,social_label(pinfo.slug),IC_HEART,BS_ACCENT,0))watch_social_pressed=1;
    if(ui_button(SCR_W-140,OSD_BOT+26,132,24,"Suscribirse",0,BS_NORMAL,0))watch_social_pressed=2;
    ui_draw_toast(OSD_BOT - 6);
}

static int fs_osd_was = -1, fs_layout_was = -1, fs_chat_gen = -1;
static u64 fs_last_osd = 0, fs_last_full = 0;
static double fs_last_pts = -1;
static const tv_pic_t *fs_last_pic = NULL;
static int watch_frame(int input) {
    const tv_pic_t *pic = curpic;
    int wide = gfx_widescreen();
    u64 now = gettime();
    int lay = chat_layout();
    if (lay != fs_layout_was) { fs_layout_was = lay; fs_osd_was = -1; fs_chat_gen = -1; fs_last_pic = NULL; fs_last_osd = 0; input = 1; }
    if (lay) {
        /* video arriba a la izquierda, datos debajo, chat a la derecha (estilo twitch.tv) */
        int vw = CHAT_X, vh = vw*9/16;
        int g = chat_gen();
        int has = pic && tp_has_video();
        int dt=ticks_to_millisecs(now-fs_last_osd);
        int ui_dirty=input||(g!=fs_chat_gen&&dt>=250)||dt>=(has?1000:100);
        if (ui_dirty) {
            gfx_rect(0, 0, vw, vh, 0x000000);
            if (!has) {
                const kimg_t *th = thumb_get(pinfo.thumb);
                if (th) { gfx_blit_scaled(th->px, th->w, th->h, 0, 0, vw, vh); gfx_rect_a(0, 0, vw, vh, 0, 150); }
                if (tp_state() == TP_PLAYING) { gfx_text_c(&font_bold, vw / 2, vh / 2 - 10, "Solo audio", C_DIM); draw_vu(vw / 2 - 100, vh / 2 + 14, 200, 10); }
                else if (tp_state() != TP_ERROR) { ui_spinner(vw / 2, vh / 2 - 8, 16); gfx_text_c(&font_small, vw / 2, vh / 2 + 16, tp_status(), C_DIM); }
                else gfx_text_c(&font_small, vw / 2, vh / 2, tp_error(), C_LIVE);
            }
            draw_info_panel(vh);
            draw_chat_panel();
            gfx_ui_cache_rows(0, SCR_H);
            fs_chat_gen = g; fs_last_osd = now;
        }
        /* rc8: emotes animados: sólo sus filas, sin redibujar el panel (<=12,5 cuadros/s) */
        int anim = !ui_dirty && chat_anim_tick() > 0;
        int newpic = has && (pic != fs_last_pic || pic->pts != fs_last_pts);
        if (!newpic && !ui_dirty && !anim) return 0;
        if (has) { fs_last_pic = pic; fs_last_pts = pic->pts; }
        memset(rowsrc, 1, sizeof(rowsrc));
        gfx_composite(has ? pic : NULL, 0, 0, vw, vh, rowsrc);
        return 1;
    }
    int vw = SCR_W, vh = wide ? SCR_H : 360, vy = (SCR_H - vh) / 2;
    chat_anim_reset();   /* rc8: chat oculto: nada que animar */
    int osd = ticks_to_millisecs(now - osd_t) < 5000 || tp_state() != TP_PLAYING;
    if (!pic || !tp_has_video()) {
        if (!input && ticks_to_millisecs(now - fs_last_full) < 33) return 0;
        fs_last_full = now;
        gfx_clear(0x000000);
        const kimg_t *th = thumb_get(pinfo.thumb);
        if (th) { gfx_blit_scaled(th->px, th->w, th->h, 0, 60, SCR_W, 360); gfx_rect_a(0, 60, SCR_W, 360, 0, 150); }
        if (tp_state() == TP_PLAYING && !tp_has_video()) {
            gfx_text_c(&font_bold, SCR_W / 2, 230, "Solo audio", C_DIM);
            draw_vu(SCR_W / 2 - 120, 260, 240, 14);
        } else if (tp_state() != TP_ERROR) {
            ui_spinner(SCR_W / 2, 230, 18);
            gfx_text_c(&font_small, SCR_W / 2, 262, tp_status(), C_DIM);
        }
        draw_osd();
        gfx_ui_cache_rows(0, SCR_H);
        gfx_composite(NULL, 0, 0, 0, 0, NULL);
        fs_osd_was = -1;
        return 1;
    }
    int osd_dirty = 0;
    if (osd && (osd != fs_osd_was || input || ticks_to_millisecs(now - fs_last_osd) >= 250)) {
        gfx_rect(0, 0, SCR_W, OSD_TOP, 0); gfx_rect(0, OSD_BOT, SCR_W, SCR_H - OSD_BOT, 0);
        draw_osd();
        gfx_ui_cache_rows(0, OSD_TOP);
        gfx_ui_cache_rows(OSD_BOT, SCR_H);
        fs_last_osd = now; osd_dirty = 1;
    }
    if (osd != fs_osd_was) osd_dirty = 1;
    fs_osd_was = osd;
    int newpic = pic != fs_last_pic || pic->pts != fs_last_pts;
    if (!newpic && !osd_dirty) return 0;
    fs_last_pic = pic; fs_last_pts = pic->pts;
    memset(rowsrc, 0, sizeof(rowsrc));
    if (osd) { memset(rowsrc, wide ? 2 : 1, OSD_TOP); memset(rowsrc + OSD_BOT, wide ? 2 : 1, SCR_H - OSD_BOT); }
    gfx_composite(pic, 0, vy, vw, vh, rowsrc);
    return 1;
}

/* ---------------------------------------------------------------- ajustes */
static const char *help_text =
    "CONTROLES\n"
    "Puntero + A: elegir tarjeta, pesta\xc3\xb1" "a o bot\xc3\xb3n.  Cruceta: moverse.\n"
    "1: buscar (teclado en pantalla).  2: actualizar.  -: cambiar idioma.  B: ajustes / volver.\n"
    "Viendo un directo: B vuelve al canal, Izq/Der volumen, +: chat a la derecha / pantalla completa, "
    "1 escribir, - seguir, 2 suscribirse, puntero+A en los botones del chat, A muestra u oculta la barra en pantalla completa, HOME salir.\n"
    "Tambi\xc3\xa9n sirven el Classic Controller, el mando de GameCube y un teclado USB.\n"
    "Iniciar sesi\xc3\xb3n: bot\xc3\xb3n violeta en la barra de arriba (QR, app WiiStream). Tambi\xc3\xa9n est\xc3\xa1 en Ajustes.\n\n"
    "SERVIDOR COMPANION (recomendado)\n"
    "En una PC de tu misma red corr\xc3\xa9: python3 twitchwii_server.py  y pon\xc3\xa9 su IP en Ajustes > Servidor "
    "(se guarda en sd:/apps/TwitchWii/server.txt). Con el servidor ten\xc3\xa9s miniaturas, avatares, box art, "
    "chat de solo lectura, videos (VODs) y video por HTTP sin TLS (Proxy 360p) o transcodificado a 270p con ffmpeg.\n\n"
    "SIN SERVIDOR\n"
    "La Wii habla directo con Twitch por HTTPS, "
    "con miniaturas, avatares, box art, banners, VODs publicos y chat anonimo TCP/TLS/WSS. VOD desde el comienzo, sin seek/resume. Tolera minutos de diferencia del reloj; conserva la fecha aproximada correcta.\n\n"
    "ANUNCIOS\n"
    "Twitch puede insertar anuncios al empezar un directo; se ven igual que en la web sin cuenta (no se saltan).\n\n"
    "FILTRO\n"
    "Por defecto se ocultan los canales +18 y las categor\xc3\xad" "as de apuestas/casino.";

static const char *about_text =
    "TwitchWii " TV_VERSION " - cliente NO OFICIAL de twitch.tv hecho por fans.\n\n"
    "NO est\xc3\xa1 afiliado, autorizado ni respaldado por Twitch (Twitch Interactive, Inc.) ni por Nintendo. "
    "\"Twitch\" es marca de sus due\xc3\xb1os y se usa solo para decir con qu\xc3\xa9 servicio funciona. "
    "El \xc3\xad" "cono de HBC es arte propio.\n\n"
    "Solo muestra transmisiones y videos p\xc3\xba" "blicos, igual que twitch.tv sin iniciar sesi\xc3\xb3n. "
    "Login OAuth opcional para escribir. No accede a contenido para suscriptores. "
    "Los contenidos pertenecen a sus creadores.\n\n"
    "M\xc3\xbasica de la intro: Claude Debussy, R\xc3\xaaverie (arr. saxof\xc3\xb3n soprano y piano), por David Hernando Vitores, CC BY-SA 4.0 (Wikimedia Commons). "
    "Imagen de la intro: splash elegido por el usuario.\n\n"
    "Software: FFmpeg/libavcodec/libavformat (LGPL 2.1), libcurl, mbedTLS, zlib, libogc, fuente DejaVu Sans. "
    "Reproductor, gr\xc3\xa1" "ficos y red reutilizados de KickWii.";

static void edit_server(void) {
    char b[80];
    const char *s = k_server;
    if (!strncmp(s, "http://", 7)) s += 7;
    snprintf(b, sizeof(b), "%s", s);
    if (!sui_text_input("Servidor companion", "IP de la PC con twitchwii_server.py, ej: 192.168.0.10 8788 (espacio = ':'). Vac\xc3\xad" "o = directo a Twitch", b, sizeof(b) - 1)) return;
    char *p = b; while (*p == ' ') p++;
    int n = strlen(p); while (n && p[n - 1] == ' ') p[--n] = 0;
    char *sp = strchr(p, ' '); if (sp) { *sp = ':'; }
    if (!*p) k_server[0] = 0;
    else snprintf(k_server, sizeof(k_server), "http://%s%s", p, strchr(p, ':') ? "" : ":8788");
    cfg_save();
    thumbs_flush();
    check_server();
    load_live(1);
}


static const char *display_title(void) {
    return k_lang==2?"Screen type":k_lang==3?"Tipo de tela":"Tipo de pantalla";
}
static const char *display_choice(int mode) {
    static const char *es[]={"Automático (según la Wii)","4:3 (pantalla cuadrada)","16:9 (pantalla plana/panorámica)"};
    static const char *en[]={"Automatic (Wii setting)","4:3 (square screen)","16:9 (flat/widescreen)"};
    static const char *pt[]={"Automático (conforme o Wii)","4:3 (tela quadrada)","16:9 (tela plana/panorâmica)"};
    if(mode<0||mode>2)mode=0;
    return k_lang==2?en[mode]:k_lang==3?pt[mode]:es[mode];
}
static void display_settings(void) {
    int choice=gfx_aspect_mode();
    char *items[3]={(char *)display_choice(0),(char *)display_choice(1),(char *)display_choice(2)};
    u32 r=sui_list(display_title(),k_lang==2?"A: apply   B: back":k_lang==3?"A: aplicar   B: voltar":"A: aplicar   B: volver",items,3,&choice);
    if(r==WPAD_BUTTON_HOME){quit=1;return;}
    if(r!=WPAD_BUTTON_A)return;
    gfx_set_aspect(choice);cfg_save();
    sel=scroll=scroll_t=bsel=bscroll=bscroll_t=0;
}

static void settings(void) {
    int sel_i = 0;
    while (!quit) {
        char it[11][160];
        char *items[11];
        int i;
        snprintf(it[0], 160, "Servidor companion\n%s", k_server[0] ? k_server : "ninguno: miniaturas y chat directos");
        snprintf(it[1], 160, "Modo de video\n%s", k_server[0] ? pm_names[k_play_mode] : "Directo (sin servidor, 360p HTTPS)");
        snprintf(it[2], 160, "Idioma de los directos\n%s", k_lang_names[k_lang]);
        snprintf(it[3], 160, "Chat al ver un directo\n%s", k_chat_on ? "S\xc3\xad (a la derecha, solo lectura)" : "No (pantalla completa)");
        snprintf(it[4], 160, "Filtro +18 / apuestas\n%s", k_show_mature ? "Desactivado (se muestra todo)" : "Activado (recomendado)");
        snprintf(it[5], 160, "Probar conexi\xc3\xb3n\n%s", !k_server[0] ? "modo directo" : (k_server_ok ? "\xc3\xbaltima prueba: OK" : "\xc3\xbaltima prueba: sin respuesta"));
        snprintf(it[6], 160, "Ayuda");
        snprintf(it[7], 160, "Acerca de (no oficial)");
        snprintf(it[8], 160, "Salir al Homebrew Channel");
        snprintf(it[9], 160, "%s\n%s", display_title(), display_choice(gfx_aspect_mode()));
        snprintf(it[10],160,"Cuenta / Iniciar sesi\xc3\xb3n\n%s",auth_status());
        items[0]=it[10];
        items[1]=it[9];
        for (i=0;i<9;i++)items[i+2]=it[i];
        u32 r = sui_list("Ajustes", "A: elegir   B: volver", items, 11, &sel_i);
        if (r == WPAD_BUTTON_HOME) { quit = 1; return; }
        if (r != WPAD_BUTTON_A) return;
        switch (sel_i==0?10:sel_i==1?9:sel_i-2) {
        case 10: chat_stop();auth_screen();thumbs_pause(0);break;
        case 9: display_settings(); break;
        case 0: edit_server(); break;
        case 1: k_play_mode = (k_play_mode + 1) % PM_N; cfg_save(); break;
        case 2: next_language(); load_live(1); break;
        case 3: k_chat_on = !k_chat_on; cfg_save(); break;
        case 4:
            if (!k_show_mature) {
                if (!sui_confirm("Filtro", "\xc2\xbfMostrar canales +18 y de apuestas/casino? Solo para adultos.")) break;
                k_show_mature = 1;
            } else k_show_mature = 0;
            cat_sel = sel = scroll = scroll_t = 0;
            cfg_save(); rebuild_filter(); break;
        case 5: check_server(); if (k_server_ok) ui_toast("Servidor OK"); break;
        case 6: sui_text_view("Ayuda", "A/B: volver", help_text); break;
        case 7: sui_text_view("Acerca de", "A/B: volver", about_text); break;
        case 8: quit = 1; return;
        }
    }
}

/* ---------------------------------------------------------------- prueba automatica (Dolphin) */
#ifdef AUTOTEST
static u64 at_ts = 0;
static int at_step = 0;
static int at_ms(void) { return (int)ticks_to_millisecs(gettime() - at_ts); }
static void at_stats(void) {
    static u64 last = 0;
    {   /* rc14/rc8: memoria cada 10 s (mallinfo + arenas + imagenes/animaciones) */
        static u64 mlast = 0; static int pc0 = 0; static unsigned r0 = 0, t0a = 0;
        if (atlog && (!mlast || ticks_to_millisecs(gettime() - mlast) >= 10000)) {
            char mm[300]; unsigned rr, tt; thumbs_mem(mm, sizeof mm); chat_anim_stats(&rr, &tt);
            int el = mlast ? (int)ticks_to_millisecs(gettime() - mlast) : 1;
            LOG("mem: t=%us %s | vis_anim=%d anim_ticks/s=%.1f anim_px/s=%.0f presents/s=%.1f video_fps=%.1f screen=%d",
                (unsigned)ticks_to_secs(gettime() - t_start), mm, chat_anim_count(), (tt - t0a) * 1000.0 / el, (rr - r0) * 1000.0 / el,
                (gfx_present_count - pc0) * 1000.0 / el, tp_fps(), screen);
            mlast = gettime(); pc0 = gfx_present_count; r0 = rr; t0a = tt;
#ifdef MEMTRACE
            { static int mt = 0; extern void memtrace_dump(FILE *, const char *); char tg[24]; snprintf(tg, sizeof tg, "t=%us", (unsigned)ticks_to_secs(gettime() - t_start)); if (mt++ % 6 == 0) memtrace_dump(atlog, tg); }
#endif
        }
    }
    if (!atlog || screen != SCR_WATCH || ticks_to_millisecs(gettime() - last) < 2000) return;
    char st[200]; tp_stats(st, sizeof(st));
    last = gettime();
    tchat_t m[1]; int nm = chat_get(m, 1);
    LOG("watch: state=%d %s | frames=%d audio=%d video=%d | %s | chat=[%s] %s: %s | err=%s", tp_state(), tp_status(), tp_frames(), tp_has_audio(), tp_has_video(), st,
        chat_status(), nm ? m[0].name : "", nm ? m[0].text : "", tp_error());
}
static void at_next(void) { at_step++; at_ts = gettime(); LOG("step %d (screen %d)", at_step, screen); }
static void at_ptr(int x, int y) { in_set_ptr(x, y, 1); }
static void at_type(const char *s) { while (*s) in_inject_key(*s++); }
static int at_wait(int scr, int ms) { return screen == scr && at_ms() > ms; }
u32 at_hook(void) {
    u32 b = 0;
    if (!at_ts) at_ts = gettime();
    if (at_block) { at_ts = gettime(); return 0; }
#ifdef AUTOTEST_LANGUAGE
    static int phase=0, cycles=0, moves=0;
    if (phase==0 && live_loaded && at_ms()>15000) {
        in_set_ptr(MX+30,CHIP_Y+10,1); phase=1; at_next();
    } else if (phase==1 && at_ms()>600) {
        LOG("LANG click %d before=%s n=%d filter=%d",cycles+1,k_langs[k_lang],live.n,nflt);
        b=(cycles%2) ? WPAD_BUTTON_MINUS : WPAD_BUTTON_A;
        phase=2;at_next();
    } else if (phase==2 && at_ms()>8000) {
        LOG("LANG stable %d language=%s n=%d filter=%d pending=%d error=%s",cycles+1,k_langs[k_lang],live.n,nflt,thumbs_pending(),live_err);
        in_set_ptr(0,0,0);phase=3;moves=0;at_next();
    } else if (phase==3 && at_ms()>320) {
        b=WPAD_BUTTON_DOWN;at_next();if(++moves==8){phase=4;moves=0;}
    } else if (phase==4 && at_ms()>320) {
        b=WPAD_BUTTON_UP;at_next();if(++moves==8){phase=5;}
    } else if (phase==5 && at_ms()>1800) {
        if(++cycles>=12){phase=6;b=WPAD_BUTTON_2;at_next();}
        else {phase=1;in_set_ptr(MX+30,CHIP_Y+10,1);at_next();}
    } else if (phase==6 && at_ms()>8000) {
        LOG("LANG PASS: 12 changes, 3 complete cycles, 192 scroll inputs, refresh; language=%s n=%d filter=%d",k_langs[k_lang],live.n,nflt);
        quit=1;at_next();
    }
#elif defined(AUTOTEST_CATS)
    static int phase=0, downs=0, last_n=0, pages=0, pulse=0, typed=0, downs_set=0;
    static u64 p3t=0;
    if (phase==0) {
        if (!live_loaded && at_ms()>400) b=WPAD_BUTTON_A;
        if (screen==SCR_HOME && live_loaded && nhome_cats>0 && at_ms()>6000) {
            LOG("HOME shelf cats=%d live=%d", nhome_cats, live.n);
            phase=1; at_next();
        }
    } else if (phase==1) {
        int ex = 52 + gfx_text_w(&font_bold, "Inicio") + 22 + 24;
        at_ptr(ex, 16);
        if (at_ms()>2000) { b=WPAD_BUTTON_A; phase=2; at_next(); }
    } else if (phase==2) {
        if (screen==SCR_BROWSE && bloaded && bcats.n>=20 && at_ms()>5000) {
            LOG("BROWSE p1 n=%d more=%d", bcats.n, bcats_more);
            in_set_ptr(0,0,0);
            downs=0; last_n=bcats.n; pages=1; pulse=0; p3t=0;
            phase=3; at_next();
        }
    } else if (phase==3) {
        if (!p3t) p3t=gettime();
        if (bcats.n > last_n) {
            LOG("BROWSE more n=%d more=%d downs=%d pages=%d", bcats.n, bcats_more, downs, pages+1);
            last_n=bcats.n; pages++;
        }
        if (!bcats_loading && at_ms()-pulse>=380) {
            pulse=at_ms();
            b=WPAD_BUTTON_DOWN;
            downs++;
        }
        if ((!bcats_loading && pages>=5 && at_ms()>4000) || ticks_to_millisecs(gettime()-p3t)>110000) {
            LOG("BROWSE deep n=%d pages=%d downs=%d more=%d", bcats.n, pages, downs, bcats_more);
            phase=4; at_next();
        }
    } else if (phase==4) {
        if (at_ms()>5000) {
            if (bsel>=bcats.n) bsel=bcats.n-1;
            if (bsel<0) bsel=0;
            in_set_ptr(0,0,0);
            LOG("OPENCAT sel=%d/%d %s", bsel, bcats.n, (bsel>=0&&bsel<bcats.n)?bcats.v[bsel].name:"?");
            b=WPAD_BUTTON_A;
            phase=5; at_next();
        }
    } else if (phase==5) {
        if (screen==SCR_HOME && cur_game[0] && live_loaded && at_ms()>8000) {
            LOG("CATLIVE game=%s n=%d", cur_game, live.n);
            phase=6; at_next();
        } else if (at_ms()>20000) {
            LOG("CATLIVE timeout screen=%d game=%s n=%d", screen, cur_game, live.n);
            phase=6; at_next();
        }
    } else if (phase==6) {
        if (at_ms()>2000) { in_set_ptr(0,0,0); b=WPAD_BUTTON_B; phase=7; at_next(); }
    } else if (phase==7) {
        if (screen==SCR_BROWSE) {
            at_ptr(MX + MW - 118 + 50, TOP_H + 8 + 13);
            if (at_ms()>2000) { b=WPAD_BUTTON_A; typed=0; phase=8; at_next(); }
        } else if (at_ms()>8000) { phase=10; at_next(); }
    } else if (phase==8) {
        if (!typed && at_ms()>800) { at_type("fortnite"); typed=1; LOG("typed fortnite"); }
        if (typed && at_ms()>4000) { b=WPAD_BUTTON_PLUS; phase=9; at_next(); }
    } else if (phase==9) {
        if (at_ms()>8000) { in_set_ptr(0,0,0); phase=10; at_next(); }
    } else if (phase==10) {
        if (at_ms()>1000 && at_ms()<1500) { in_set_ptr(0,0,0); b=WPAD_BUTTON_B; }
        if (at_ms()>2800) { in_set_ptr(0,0,0); b=WPAD_BUTTON_B; downs_set=0; pulse=0; phase=11; at_next(); }
    } else if (phase==11) {
        if (downs_set<10 && at_ms()-pulse>=450) { pulse=at_ms(); b=WPAD_BUTTON_DOWN; downs_set++; }
        else if (downs_set>=10 && at_ms()-pulse>=800) { in_set_ptr(0,0,0); b=WPAD_BUTTON_A; phase=12; at_next(); }
    } else if (phase==12) {
        if (at_ms()>12000) { b=WPAD_BUTTON_B; phase=13; at_next(); }
    } else if (phase==13) {
        if (at_ms()>2000) { b=WPAD_BUTTON_B; phase=14; at_next(); }
    } else if (phase==14) {
        if (at_ms()>1500) { LOG("CATS autotest done n=%d pages=%d", bcats.n, pages); quit=1; }
    }
#elif defined(AUTOTEST_HOMEKEY)
    /* HOME en la pantalla de categorias */
    switch(at_step){
#ifdef HK_STAY_HOME
    case 0: if(at_wait(SCR_HOME,14000)){at_next();LOG("homekey: inicio, inyecto HOME");b=WPAD_BUTTON_HOME;}break;
#else
    case 0: if(at_wait(SCR_HOME,9000)){at_next();screen=SCR_BROWSE;if(!bloaded||!bcats.n)load_browse();LOG("homekey: pantalla categorias n=%d",bcats.n);}break;
    case 1: if(at_wait(SCR_BROWSE,8000)){at_next();LOG("homekey: inyecto HOME");b=WPAD_BUTTON_HOME;}break;
#endif
    case 2: if(at_ms()>15000){at_next();quit=1;}break;
    }
#elif defined(AUTOTEST_VOD_ONLY)
    switch(at_step){
    case 0:if(at_wait(SCR_HOME,6000)&&live_loaded){at_next();open_channel(NULL,"auronplay");}break;
    case 1:if(at_wait(SCR_CHAN,9000)){at_next();start_vod(0);}break;
    case 2:if(at_wait(SCR_WATCH,45000)){LOG("autotest VOD rc3 end");at_next();quit=1;}break;
    }
#elif defined(AUTOTEST_PICKER)
    /* rc8: selector de emotes en el teclado del chat; el envio es SEND_DRYRUN (no sale nada) */
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,8000)&&live_loaded){at_next();open_channel(NULL,AT_CHAN);}break;
    case 1: if(at_wait(SCR_CHAN,6000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,25000)){at_next();LOG("picker: abrir teclado");at_ptr(6,474);b=WPAD_BUTTON_1;}break;
    case 3: if(at_ms()>3000){at_next();at_type("hola ");}break;
    case 4: if(at_ms()>1500){at_next();LOG("picker: tecla 2 -> grilla");b=WPAD_BUTTON_2;}break;
    case 5: if(at_ms()>14000){at_next();LOG("picker: shot grilla canal (t=%.0f)",ticks_to_millisecs(gettime()-t_start)/1000.0);}break;
    case 6: if(at_ms()>4000){at_next();b=WPAD_BUTTON_PLUS;LOG("picker: + -> globales");}break;
    case 7: if(at_ms()>9000){at_next();LOG("picker: shot grilla globales (t=%.0f)",ticks_to_millisecs(gettime()-t_start)/1000.0);}break;
    case 8: if(at_ms()>3000){at_next();b=WPAD_BUTTON_RIGHT;}break;
    case 9: if(at_ms()>600){at_next();at_ptr((SCR_W-600)/2+20+3*50+24,30+102+50+24);LOG("picker: puntero sobre la celda 4 de la fila 2");}break;
    case 10: if(at_ms()>1200){at_next();b=WPAD_BUTTON_A;LOG("picker: A (puntero) -> insertar");}break;
    case 11: if(at_ms()>2500){at_next();at_ptr(6,474);at_type("y ");}break;
    case 12: if(at_ms()>1500){at_next();b=WPAD_BUTTON_2;}break;
    case 13: if(at_ms()>5000){at_next();b=WPAD_BUTTON_MINUS;}break;
    case 14: if(at_ms()>4000){at_next();b=WPAD_BUTTON_A;}break;
    case 15: if(at_ms()>4000){at_next();LOG("picker: shot vista previa (t=%.0f)",ticks_to_millisecs(gettime()-t_start)/1000.0);}break;
    case 16: if(at_ms()>4000){at_next();b=WPAD_BUTTON_PLUS;LOG("picker: + -> Enviar (dry run)");}break;
    case 17: if(at_ms()>5000){at_next();LOG("picker end");quit=1;}break;
    }
#elif defined(AUTOTEST_RC11)
    /* rc11: emotes en el mensaje propio + pruebas del vigilante (GUARD_TEST) */
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,8000)&&live_loaded){at_next();open_channel(NULL,AT_CHAN);}break;
    case 1: if(at_wait(SCR_CHAN,6000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,4000)){at_next();if(!watch_details){b=WPAD_BUTTON_PLUS;}LOG("rc11: chat visible");}break;
#ifdef AUTOTEST_OWNEMOTE
    case 3: if(at_ms()>25000){extern void auth_test_own_echo(const char*,const char*);at_next();auth_test_own_echo(pinfo.slug,"hola Kappa que buen stream LUL PogChamp");LOG("rc11: eco propio inyectado");}break;
    case 4: if(at_ms()>12000){at_next();LOG("rc11: shot emotes propios (t=%.0f)",ticks_to_millisecs(gettime()-t_start)/1000.0);}break;
    case 5: if(at_ms()>15000){at_next();quit=1;}break;
#elif defined(GUARD_TEST)
    case 3: if(at_ms()>25000){at_next();LOG("rc11: bloqueo simulado del hilo principal (esperando HOME de xdotool)");guard_test_hang(1);}break;
#endif
    }
#elif defined(AUTOTEST_SOAK)
    /* rc14/rc8: soak de memoria: canal con chat real + 3 emotes animados distintos cada 2,5 s */
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,8000)&&live_loaded){at_next();open_channel(NULL,AT_CHAN);}break;
    case 1: if(at_wait(SCR_CHAN,6000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,2500)){static int k=0;static u64 s0=0;if(!s0)s0=gettime();at_ts=gettime();chat_test_soak(k++);
             if(ticks_to_millisecs(gettime()-s0)>=(u64)AT_SOAK_MIN*60000){LOG("soak end k=%d",k);at_next();quit=1;}}break;
    }
#elif defined(AUTOTEST_EMOTES)
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,8000)&&live_loaded){at_next();open_channel(NULL,AT_CHAN);}break;
    case 1: if(at_wait(SCR_CHAN,6000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,6000)){at_next();chat_test_inject();LOG("emotes: mensajes de prueba inyectados");}break;
    case 3: if(at_wait(SCR_WATCH,3000)){static int k=0;at_ts=gettime();chat_test_inject();if(++k>=40){at_next();quit=1;}}break;
    }
#else
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,10000)&&live_loaded){at_next();open_channel(NULL,"twitchplayspokemon");}break;
    case 1: if(at_wait(SCR_CHAN,9000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,55000)){at_next();stop_watch();screen=SCR_CHAN;}break;
    case 3: if(at_wait(SCR_CHAN,2000)){at_next();open_channel(NULL,"auronplay");}break;
    case 4: if(at_wait(SCR_CHAN,9000)){at_next();start_vod(0);}break;
    case 5: if(at_wait(SCR_WATCH,45000)){LOG("autotest rc3 end");at_next();quit=1;}else if(screen==SCR_WATCH&&at_ms()%10000<35)b=WPAD_BUTTON_UP;break;
    }
#endif
    return b;
}
#endif

/* ---------------------------------------------------------------- principal */
static int last_px = -1, last_py = -1, last_pv = 0, last_screen = -1;
static u64 last_full = 0;

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    twitchwii_mem2_init();
#ifdef MEMTRACE
    { extern void memtrace_start(void); memtrace_start(); }
#endif
    gfx_init();
    input_init();
    fat_ok = fatInitDefault();
    tp_init();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (fat_ok) cfg_load();
    t_start = gettime();
    dlog_init(fat_ok);   /* rc11: log de diagnostico en sd:/apps/TwitchWii/log.txt (como KickWii) */
    dlog("TwichWii " TV_VERSION " | IOS %d | pantalla %s | SD %s", IOS_GetVersion(), gfx_widescreen() ? "16:9" : "4:3", fat_ok ? "ok" : "no");
    guard_init();        /* rc11: vigilante del hilo principal (HOME de emergencia, log, corta sockets) */
    {
        extern unsigned char __bss_end[];
        u32 a1 = (u32)SYS_GetArena1Hi() - (u32)SYS_GetArena1Lo();
        u32 a2 = (u32)SYS_GetArena2Hi() - (u32)SYS_GetArena2Lo();
        printf("MEM1 __bss_end %p arena1 %lu KB | MEM2 TX %08lx-%08lx (%lu KB) canvas %p arena2 %lu KB\n",
               (void *)__bss_end, (unsigned long)(a1 / 1024),
               (unsigned long)twitchwii_mem2_bss_start(), (unsigned long)twitchwii_mem2_bss_end(),
               (unsigned long)(twitchwii_mem2_bss_size() / 1024),
               (void *)canvas, (unsigned long)(a2 / 1024));
    }
#ifdef AUTOTEST
    mkdir("sd:/twitchwii-test", 0777);
    atlog = fopen("sd:/twitchwii-test/autotest.log", "w");
    LOG("autotest start server=%s", k_server);
#endif
    auth_init();
    thumbs_init();
    ui_set_backdrop(draw_home);
    rebuild_filter();
    int net = intro_run();
#ifdef KBD_BENCH
    { char kbq[40] = ""; ui_set_backdrop(draw_home); sui_text_input("Buscar", "Escribi el nombre de un canal", kbq, sizeof(kbq) - 1); quit = 1; }
#endif
    if (!net) ui_toast("La red no est\xc3\xa1 lista; toc\xc3\xa1 Actualizar para reintentar.");
    else {
        check_server();
        if(auth_due())auth_ensure();
        load_live(1);
        load_home_cats();
    }
    while (SYS_MainLoop() && !quit) {
        grid_y_extra = (screen == SCR_HOME && !cur_game[0] && nhome_cats) ? SHELF_H : 0;
        if(chat_auth_rejected()){chat_stop();auth_refresh_now();watch_chat_started=0;} /* rc12: renovar en vez de borrar la sesion guardada */
        guard_where("loop: log");
        dlog_flush(0);
        /* rc11: la revision periodica de la sesion ya no corre mientras se mira un directo (cortaba el
           chat y hacia un pedido HTTP bloqueante en el hilo principal); se hace al salir del directo. */
        if(screen!=SCR_WATCH&&auth_due()){guard_where("loop: auth_ensure");chat_stop();thumbs_pause(1);auth_ensure();thumbs_pause(0);}
        guard_where("loop: botones");

        u32 d = in_poll();
        int k;
        intro_music_tick();
        while ((k = in_key()) != 0) {
            if (k == 13) d |= WPAD_BUTTON_A;
            else if (k == 27) d |= WPAD_BUTTON_B;
            else if (k == 's' || k == '/') d |= WPAD_BUTTON_1;
        }
        if (d & WPAD_BUTTON_HOME) break;
        ui_begin(d);
        h_act = HA_NONE; s_act = 0; c_act = 0; b_act = 0; sb_pick = NULL;

        if (screen == SCR_WATCH) {
#ifdef AUTOTEST
            at_stats();
#endif
            ui_set_backdrop(NULL);
            thumbs_pause(1);

            if (d) osd_t = gettime();
            if (d & WPAD_BUTTON_PLUS) {
                if (!watching_vod) {
                    watch_details = !watch_details;
                    /* One bounded read-only worker per playback. No SD writes,
                       joins, decoder or framebuffer reallocation on +. */
                    if (watch_details && !watch_chat_started) {
                        watch_chat_started = 1; chat_start(pinfo.slug);
                    }
                }
                else ui_toast(watching_vod ? "Los VODs no tienen chat." : "Los VODs no tienen chat.");
            }
            guard_where("watch: tp_picture");
            curpic = tp_picture();
            guard_where("watch: dibujo");
            int wrote = watch_frame(d != 0);
            guard_where("watch: acciones");
            if(watch_social_pressed||(d&(WPAD_BUTTON_MINUS|WPAD_BUTTON_2))){int sub=watch_social_pressed?watch_social_pressed==2:!!(d&WPAD_BUTTON_2);watch_social_pressed=0;chat_stop();social_screen(pinfo.slug,sub);if(watch_chat_started)chat_start(pinfo.slug);thumbs_pause(1);fs_layout_was=fs_osd_was=-1;fs_last_pic=NULL;osd_t=gettime();curpic=NULL;continue;}
            if(chat_write_pressed||(d&WPAD_BUTTON_1)){chat_write_pressed=0;if(!auth_ready()){chat_stop();auth_screen();if(watch_chat_started)chat_start(pinfo.slug);}if(auth_ready()&&!watch_chat_started){watch_chat_started=1;chat_start(pinfo.slug);}if(auth_ready())auth_compose(pinfo.slug,0);thumbs_pause(1);fs_layout_was=fs_osd_was=-1;fs_last_pic=NULL;osd_t=gettime();curpic=NULL;continue;}

            if (!wrote) VIDEO_WaitVSync();
            if (d & WPAD_BUTTON_B) { stop_watch(); screen = SCR_CHAN; }
            if (d & WPAD_BUTTON_LEFT) { tp_set_volume(tp_volume() - 24); osd_t = gettime(); }
            if (d & WPAD_BUTTON_RIGHT) { tp_set_volume(tp_volume() + 24); osd_t = gettime(); }
            if (d & WPAD_BUTTON_A) osd_t = ticks_to_millisecs(gettime() - osd_t) < 5000 ? 0 : gettime();
            continue;
        }

        u64 now = gettime();
        int moved = abs(in_ptr.x - last_px) + abs(in_ptr.y - last_py) >= 2 || in_ptr.valid != last_pv;
        int ms = ticks_to_millisecs(now - last_full);
        int dirty = d || moved || scroll != scroll_t || bscroll != bscroll_t || screen != last_screen || ms >= 250;
        if (!dirty) { VIDEO_WaitVSync(); continue; }
        thumbs_frame();
        if (screen == SCR_HOME) { ui_set_backdrop(draw_home); draw_home(); }
        else if (screen == SCR_BROWSE) { ui_set_backdrop(draw_browse); draw_browse(); }
        else if (screen == SCR_SEARCH) { ui_set_backdrop(draw_search); draw_search(); }
        else { ui_set_backdrop(draw_channel); draw_channel(); }
        ui_present();
        last_full = now; last_px = in_ptr.x; last_py = in_ptr.y; last_pv = in_ptr.valid; last_screen = screen;
        if (UI.consumed) d &= ~WPAD_BUTTON_A;

        int tb = 0;
        if (h_act >= 100) tb = h_act - 100;
        if (s_act >= 100) tb = s_act - 100;
        if (c_act >= 100) tb = c_act - 100;
        if (b_act >= 100) tb = b_act - 100;
        if (tb == TB_HOME) { go_home(); continue; }
        if (tb == TB_BROWSE) { screen = SCR_BROWSE; if (!bloaded || !bcats.n) load_browse(); continue; }
        if (tb == TB_SEARCH || (d & WPAD_BUTTON_1)) { do_search(); continue; }
        if (tb == TB_SETTINGS) { settings(); continue; }
        if (tb == TB_ACCOUNT) { chat_stop(); auth_screen(); thumbs_pause(0); continue; }
        if (sb_pick) { char sl[48]; snprintf(sl, sizeof(sl), "%s", sb_pick); int i; const kstream_t *f = NULL;
            for (i = 0; i < side.n; i++) if (!strcmp(side.v[i].slug, sl)) f = &side.v[i];
            open_channel(f, sl); continue; }

        if (screen == SCR_HOME) {
            int ncards = nflt + (live_more ? 1 : 0);
            switch (h_act) {
            case HA_OPEN: { const kstream_t *card = filtered_stream(h_arg); sel = h_arg; if (card) open_channel(card, card->slug); continue; }
            case HA_MORE: load_live(0); continue;
            case HA_LANG: next_language(); load_live(1); continue;
            case HA_REFRESH: load_live(1); continue;
            case HA_CAT: cat_sel = h_arg; sel = 0; scroll_t = 0; rebuild_filter(); continue;
            case HA_BACKCAT: cur_game[0] = 0; screen = SCR_BROWSE; if (!bcats.n) load_browse(); continue;
            case HA_OPENCAT: if (h_arg >= 0 && h_arg < nhome_cats) { open_category(&home_cats[h_arg]); } continue;
            case HA_ALLCATS: screen = SCR_BROWSE; if (!bloaded || !bcats.n) load_browse(); continue;
            }
            if (ncards) {
                int os = sel;
                if (d & WPAD_BUTTON_LEFT)  sel = sel > 0 ? sel - 1 : 0;
                if (d & WPAD_BUTTON_RIGHT) sel = sel < ncards - 1 ? sel + 1 : sel;
                if (d & WPAD_BUTTON_UP)    sel = sel >= nflt ? (nflt ? nflt - 1 : 0) : (sel >= COLS ? sel - COLS : sel);
                if (d & WPAD_BUTTON_DOWN)  sel = sel + COLS < ncards ? sel + COLS : ncards - 1;
                if (os != sel || (d & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN))) { ensure_visible(); if (in_ptr.virt) in_ptr.valid = 0; }
                if ((d & WPAD_BUTTON_A) && !in_ptr.valid) {
                    if (sel < nflt) { const kstream_t *card = filtered_stream(sel); if (card) open_channel(card, card->slug); continue; }
                    load_live(0); continue;
                }
            }
            if (d & WPAD_BUTTON_2) load_live(1);
            if (d & WPAD_BUTTON_MINUS) { next_language(); load_live(1); }
            if (d & WPAD_BUTTON_B) { if (cur_game[0]) { cur_game[0] = 0; screen = SCR_BROWSE; if (!bcats.n) load_browse(); } else settings(); }
        } else if (screen == SCR_BROWSE) {
            int nshow = bcats.n + ((bcats_more && !cat_search_q[0]) ? 1 : 0);
            if (b_act == 1 && b_arg >= 0 && b_arg < bcats.n) { tcat_t c = bcats.v[b_arg]; open_category(&c); continue; }
            if (b_act == 2) { go_home(); continue; }
            if (b_act == 3) { load_browse_more(); continue; }
            if (b_act == 4) { cat_search_q[0] = 0; load_browse(); continue; }
            if (b_act == 5) { do_cat_search(); continue; }
            if (nshow) {
                int os = bsel;
                if (d & WPAD_BUTTON_LEFT)  bsel = bsel > 0 ? bsel - 1 : 0;
                if (d & WPAD_BUTTON_RIGHT) bsel = bsel < nshow - 1 ? bsel + 1 : bsel;
                if (d & WPAD_BUTTON_UP)    bsel = bsel >= BCOLS ? bsel - BCOLS : bsel;
                if (d & WPAD_BUTTON_DOWN)  bsel = bsel + BCOLS < nshow ? bsel + BCOLS : nshow - 1;
                if (os != bsel && in_ptr.virt) in_ptr.valid = 0;
                if ((d & WPAD_BUTTON_A) && !in_ptr.valid) {
                    if (bsel < bcats.n) { tcat_t c = bcats.v[bsel]; open_category(&c); continue; }
                    load_browse_more(); continue;
                }
            }
            if (d & WPAD_BUTTON_2) load_browse();
            if (d & WPAD_BUTTON_B) go_home();
        } else if (screen == SCR_SEARCH) {
            int extra = valid_login(search_q) ? 1 : 0, n = results.n + extra;
            if (d & WPAD_BUTTON_UP) { if (rsel > 0) rsel--; if (in_ptr.virt) in_ptr.valid = 0; }
            if (d & WPAD_BUTTON_DOWN) { if (rsel < n - 1) rsel++; if (in_ptr.virt) in_ptr.valid = 0; }
            if (!s_act && (d & WPAD_BUTTON_A) && !in_ptr.valid && n) { s_act = 1; s_arg = rsel; }
            if (s_act == 3 && s_arg >= 0 && s_arg < search_cats.n) { open_category(&search_cats.v[s_arg]); continue; }
            if (s_act == 1) {
                if (extra && s_arg == 0) open_channel(NULL, search_q);
                else { kstream_t r = results.v[s_arg - extra]; open_channel(&r, r.slug); }
                continue;
            }
            if (d & WPAD_BUTTON_B) screen = SCR_HOME;
        } else if (screen == SCR_CHAN) {
            int nv=nvods<4?nvods:4,minb=chan.live>0?0:1;
            if((d&(WPAD_BUTTON_UP|WPAD_BUTTON_DOWN|WPAD_BUTTON_LEFT|WPAD_BUTTON_RIGHT))&&in_ptr.virt)in_ptr.valid=0;
            if(d&WPAD_BUTTON_UP)chan_focus=chan_focus>minb?chan_focus-1:minb;
            if(d&WPAD_BUTTON_DOWN)chan_focus=chan_focus<5+nv?chan_focus+1:chan_focus;
            if((d&WPAD_BUTTON_LEFT)&&chan_focus>=6)chan_focus=chan_focus>6?chan_focus-1:5;
            if((d&WPAD_BUTTON_RIGHT)&&nv)chan_focus=chan_focus<6?6:(chan_focus<5+nv?chan_focus+1:chan_focus);
            if(!c_act&&(d&WPAD_BUTTON_A)&&!in_ptr.valid){static const int actions[]={1,6,7,2,3,4};if(chan_focus<6)c_act=actions[chan_focus];else{c_act=5;c_arg=chan_focus-6;}}
            if(c_act==6||c_act==7){chat_stop();social_screen(chan.slug,c_act==7);thumbs_pause(0);continue;}
            if (c_act == 1 && chan.live > 0) { start_watch(); continue; }
            if (c_act == 2) { k_chat_on = !k_chat_on; cfg_save(); }
            if (c_act == 3) { if (k_server[0]) { k_play_mode = (k_play_mode + 1) % PM_N; cfg_save(); } else ui_toast("Configur\xc3\xa1 un servidor companion para elegir el modo."); }
            if (c_act == 5) { start_vod(c_arg); continue; }
            if (c_act == 4 || (d & WPAD_BUTTON_B)) { screen = prev_screen == SCR_SEARCH ? SCR_SEARCH : (prev_screen == SCR_BROWSE ? SCR_BROWSE : SCR_HOME); continue; }
            if (d & WPAD_BUTTON_2) { kstream_t c = chan; open_channel(&c, c.slug); }
        }
    }
    /* HOME / Salir: primero se muestra "Saliendo..." (antes se detenia todo antes de dibujar y, con las
     * miniaturas de Categorias bajando, el cierre podia quedar colgado con el cuadro a medias); despues se
     * cortan los pedidos de red, se frenan los hilos de miniaturas y la pantalla se deja en negro. */
    gfx_clear(C_BG);
    ui_logo(SCR_W / 2 - 24, 180, 48);
    gfx_text_c(&font_bold, SCR_W / 2, 250, "Saliendo...", C_DIM);
    gfx_present();
    guard_kill_socks(LWP_THREAD_NULL);
    chat_stop(); tp_stop(); playing = 0;
    thumbs_pause(1);
    dlog("salida"); dlog_flush(1);
    if (atlog) fclose(atlog);
    gfx_black_now();
    return 0;
}
