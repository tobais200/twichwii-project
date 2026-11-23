/*
 * TwitchWii - cabecera comun. Cliente NO OFICIAL (fan-made) de twitch.tv para la Wii
 * (Homebrew Channel, libogc). Interfaz estilo twitch.tv: tema oscuro #0E0E10, acento
 * violeta #9146FF, barra superior con Inicio/Explorar y buscador, "Canales recomendados"
 * a la izquierda, tarjetas de directos (miniatura, EN VIVO rojo, espectadores, avatar,
 * titulo, categoria, etiquetas), pagina de canal con videos recientes y chat de solo
 * lectura. Datos via servidor companion en la LAN (o directo a gql.twitch.tv).
 * Solo lectura de datos publicos: sin login, sin enviar mensajes, sin saltar anuncios.
 * No esta afiliado a Twitch ni a Nintendo.
 */
#ifndef TV_H
#define TV_H
#include <stddef.h>
#include <gctypes.h>
#include "gfx.h"
#include "auth.h"

#include "clock_sync.h"

#define TV_VERSION "1.2-rc7"
#define TV_UA          "Mozilla/5.0 (Nintendo Wii; U; ; 3642; es) AppleWebKit/535.19 (KHTML, like Gecko) TwitchWii/" TV_VERSION
#define TV_CFG_DIR     "sd:/apps/TwitchWii/"
#define TV_APP_NAME    "TwitchWii"
/* fsguard.c */
int fs_dev_mounted(const char *path);
int fs_data_dirs(const char *app, char dirs[2][96], int create);

/* ---- text.c ---- */
int   cp_to_ascii(unsigned cp, char *out);
char *strip_html(const char *html);

/* ---- input.c ---- */
typedef struct { int x, y, valid, virt, idle; u32 held; } in_ptr_t;
extern in_ptr_t in_ptr;
void input_init(void);
u32  in_poll(void);
int  in_key(void);
void in_inject_key(int k);
void in_inject_btn(u32 b);
void in_set_ptr(int x, int y, int valid);
#ifdef AUTOTEST
u32  at_hook(void);
#endif

/* ---- ui.c: paleta estilo twitch.tv (tema oscuro) ---- */
#define C_BG      0x0E0E10   /* fondo principal */
#define C_BG2     0x18181B   /* barra superior */
#define C_PANEL   0x1F1F23   /* barra lateral / paneles */
#define C_PANEL2  0x35353B   /* botones grises */
#define C_LINE    0x3A3A3D
#define C_TEXT    0xEFEFF1
#define C_DIM     0xADADB8
#define C_FAINT   0x7A7A85
#define C_ACCENT  0x9146FF   /* violeta Twitch */
#define C_ACCENT2 0x772CE8
#define C_ACCENT_HI 0xA970FF
#define C_LINK    0xBF94FF   /* categorias / enlaces */
#define C_LIVE    0xEB0400   /* EN VIVO rojo */
#define C_OKC     0x00F593
#define C_WARN    0xFFB23F
#define C_DARK    0x0E0E10
#define C_TAG     0x323239
extern int ui_quit;
typedef struct { u32 down; int consumed; } ui_frame_t;
extern ui_frame_t UI;
void ui_begin(u32 down);
int  ui_hover(int x, int y, int w, int h);
int  ui_click(int x, int y, int w, int h);
enum { BS_NORMAL = 0, BS_ACCENT, BS_GHOST, BS_ICON };
int  ui_button(int x, int y, int w, int h, const char *label, int icon, int style, int focus);
void ui_present(void);
void ui_draw_cursor(void);
void ui_set_backdrop(void (*fn)(void));
void ui_backdrop_suspend(int on);
void ui_backdrop(int dim);
void ui_panel(int x, int y, int w, int h, const char *title);
void ui_spinner(int cx, int cy, int r);
int  ui_wrap(const void *font, const char *text, int maxw, char ***lines);
void ui_toast(const char *msg);
void ui_draw_toast(int ybottom);
void ui_logo(int x, int y, int s);
void ui_busy_tick(void);
/* intro.c: pantalla de arranque + musica */
int  intro_run(void);
void intro_music_stop(void);
void intro_music_tick(void);
void sui_busy(const char *title, const char *msg);
void sui_busy_end(void);
u32  sui_list(const char *title, const char *help, char **items, int n, int *sel);
u32  sui_text_view(const char *title, const char *help, const char *text);
void sui_message(const char *title, const char *text);
int sui_text_input_ex(const char*,const char*,char*,int,const char*,const char*);
int  sui_text_input(const char *title, const char *hint, char *buf, int max);
int  sui_confirm(const char *title, const char *text);

/* ---- http.c ---- */
typedef struct {
    char  *data;
    size_t len, alloc, cap;
    long   code;
    int    res;
    char   err[256];
} sh_resp_t;
int   sh_net_ready(void);
int   sh_cfg_read(const char *name, char *buf, int sz);
int   sh_cfg_write(const char *name, const char *value);
char *sh_strdup(const char *s);
char *sh_html_to_text(const char *html);
void  sh_curl_common(void *curl, char *errbuf);
int   sh_request(const char *method, const char *url, const char *const *headers, const char *body, const char *ua, sh_resp_t *r);
void  sh_resp_free(sh_resp_t *r);
const char *jws(const char *p);
const char *jskip(const char *p);
const char *jget(const char *obj, const char *key);
int         jstr(const char *v, char *out, int outsz);
const char *jpath(const char *obj, const char *path);
const char *jarr_first(const char *arr);
const char *jarr_next(const char *elem);
int         jis_null(const char *v);
long long   jnum(const char *v);
char       *sh_urlencode(const char *s);
int         sh_ok(const sh_resp_t *r);

/* ---- twitchapi.c: modelo de datos + backends (companion / directo a gql.twitch.tv) ---- */
enum { SRC_TWITCH = 0 };
typedef struct {
    int  src;
    char slug[48];       /* login: twitch.tv/<login> */
    char name[48];       /* displayName */
    char title[192];     /* titulo del directo */
    char cat[48];        /* categoria (juego) */
    char catslug[48];
    char lang[20];
    char start[24];      /* ISO 8601 (UTC) del inicio */
    char shortn[4];      /* iniciales (sin miniatura) */
    char tags[3][24];    /* etiquetas libres */
    int  ntags;
    char thumb[768];     /* ruta en el companion (/img?...fmt=rgb) o "" */
    char avatar[768];
    char banner[768];
    char bio[256];
    int  viewers;
    int  followers;
    int  verified;       /* partner */
    int  live;           /* 1 en vivo, 0 offline, -1 desconocido */
    int  mature;
    int  gambling;
    int  mock;           /* datos DEMO del companion */
    u32  color;
} kstream_t;
typedef struct { kstream_t *v; int n, cap; } klist_t;
void klist_clear(klist_t *l);
kstream_t *klist_add(klist_t *l);
void kstream_style(kstream_t *s);
int  tw_is_gambling(const char *cat_name, const char *cat_slug);

typedef struct { char id[20]; char title[160]; char cat[48]; char created[24]; char thumb[768]; int length, views; } tvod_t;
typedef struct { char id[20]; char name[48]; char slug[48]; char boxart[768]; int viewers; int gambling; u32 color; } tcat_t;
typedef struct { tcat_t *v; int n, cap; } tcatlist_t;
void tcatlist_clear(tcatlist_t *l);
tcat_t *tcatlist_add(tcatlist_t *l);

enum { PM_PROXY = 0, PM_TRANSCODE, PM_DIRECT, PM_N };   /* modo de reproduccion */
extern const char *pm_names[PM_N];
extern char k_server[128];      /* "http://192.168.0.10:8788" o "" (= directo a Twitch) */
extern int  k_server_ok;
extern int  k_play_mode;
extern int  k_lang;
extern const char *k_langs[];   /* "es", "all", "en", "pt" */
extern const char *k_lang_names[];
extern int  k_nlangs;
extern int  k_show_mature;      /* 0 = ocultar +18 y apuestas (por defecto) */
extern int  k_chat_on;          /* chat de solo lectura al ver un directo */
extern int  k_last_mock;
extern char k_notice[200];
void cfg_load(void);
void cfg_save(void);
int  tw_health(char *err, int esz);
/* directos en vivo (24 por pagina). game = slug de categoria o "". after = cursor. */
int  tw_live(const char *game, char *after, int asz, klist_t *out, int *more, char *err, int esz);
void tw_categories_reset(void);
int  tw_categories_page(tcatlist_t *out, int *more, char *err, int esz);
int  tw_search_categories(const char *q, tcatlist_t *out, char *err, int esz);
int  tw_top_categories(tcat_t *out, int max, char *err, int esz);
int  tw_search(const char *q, klist_t *out, char *err, int esz);
/* 1 = en vivo, 0 = offline, -1 error. vods puede ser NULL. */
int  tw_channel(const char *login, kstream_t *s, tvod_t *vods, int maxv, int *nv, char *err, int esz);
int  tw_play_url(const kstream_t *s, char *url, int usz, char *refresh, int rsz, int *max_h, char *err, int esz);
int  tw_vod_url(const tvod_t *v, char *url, int usz, char *err, int esz);
int  tw_parse_token(const char *json, kstream_t *info, char *url, int usz);

/* ---- chat.c: chat de solo lectura (via companion), hilo propio ---- */
/* rc7: ne emotes de Twitch en text[eat[i]..+elen[i]], id en eid (tag IRC "emotes"). */
typedef struct { char name[28]; char text[200]; u32 color; int sys; unsigned char ne, eat[8], elen[8]; char eid[8][48]; } tchat_t;
int emote_url(const char *id, char *out, int cap);
void chat_test_inject(void);            /* chat.c */
int chat_rich(const void *font, int x, int y, int maxw, const tchat_t *m, u32 namecol, u32 textcol, int draw);
void chat_start(const char *login);
void chat_stop(void);
int chat_auth_rejected(void);
int  chat_gen(void);                         /* cambia cuando llega algo */
int  chat_get(tchat_t *out, int max);        /* ultimos mensajes (mas viejo primero) */
const char *chat_status(void);

/* ---- thumbs.c: miniaturas/avatares RGB del companion, en un hilo aparte ---- */
typedef struct { int w, h; u32 *px; } kimg_t;
void thumbs_init(void);
void thumbs_pause(int p);                 /* 1 = no bajar nada (video a pantalla completa) */
const kimg_t *emote_get(const char *url);  /* rc7: cola de emotes del chat */
const kimg_t *thumb_get(const char *path); /* NULL si todavia no esta; la pide */
void thumbs_flush(void);                  /* cambia el servidor: vaciar */
int  thumbs_pending(void);
void thumbs_frame(void);

/* ---- reproductor (tvplayer.c) ---- */
enum { TP_IDLE = 0, TP_CONNECTING, TP_BUFFERING, TP_PLAYING, TP_ERROR, TP_ENDED };
enum { VM_AUTO = 0, VM_AUDIO };
typedef struct {
    int w, h;
    int sw, sh;
    float aspect; /* decoded display aspect, including sample aspect ratio */
    double pts;
    u8 *y, *u, *v;
} tv_pic_t;
void tp_set_vod(int vod);
void  tp_init(void);
int   tp_play(const char *url, int vmode);
void  tp_set_limits(int max_h, const char *refresh_api_url);
void  tp_stop(void);
int   tp_state(void);
const char *tp_status(void);
const char *tp_error(void);
void  tp_set_volume(int v);
int   tp_volume(void);
const tv_pic_t *tp_picture(void);
int   tp_has_video(void);
int   tp_has_audio(void);
int   tp_vmode(void);
const char *tp_mode_text(void);
void  tp_stats(char *out, int sz);
float tp_audio_level(void);
double tp_fps(void);
int   tp_frames(void);
int   tp_dropped(void);
long long tp_bytes(void);
extern volatile u32 tp_dec_us;

/* ---- video -> pantalla (gfx.c) ---- */
void gfx_video_to_canvas(const tv_pic_t *p, int x, int y, int w, int h, int y0, int y1);
void gfx_ui_cache_rows(int y0, int y1);
void gfx_composite(const tv_pic_t *p, int vx, int vy, int vw, int vh, const unsigned char *rowsrc);
extern int gfx_present_count;
int  gfx_widescreen(void);
int gfx_aspect_mode(void);
void gfx_set_aspect(int mode);
int gfx_pointer_x(int x);
int gfx_pointer_y(int y);
#endif
