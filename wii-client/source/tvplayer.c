/*
 * tvplayer.c - reproductor de TV en vivo (HLS) para Wii:
 *   hilo de red    : libcurl (HTTP/HTTPS) baja la lista maestra, elige la variante
 *                    mas liviana, recarga la lista en vivo y baja segmentos MPEG-TS o fMP4 (Twitch)
 *                    o fMP4/CMAF (EXT-X-MAP + fragmentos moof/mdat, sin cifrado)
 *   hilo demux     : libavformat (mpegts o mov) lee los segmentos encadenados; el audio AAC
 *                    se decodifica aca mismo (libavcodec) -> 48 kHz estereo -> ASND
 *   hilo de video  : libavcodec H.264 -> cuadros YUV 4:2:0 reducidos a <= 640 px
 *                    con descarte adaptativo (B-frames / solo claves) si la CPU no da
 *   hilo principal : tp_picture() elige el cuadro segun el reloj del audio
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <unistd.h>
#include <math.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>
#include <curl/curl.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include "tv.h"

/* ------------------------------------------------------------------ audio */
#define NBLK      64
#define BLKFR     2048                    /* 42.7 ms por bloque */
#define BLKBYTES  (BLKFR * 4)
#define START_BLK 20                      /* ~0.85 s antes de empezar */
enum { B_FREE = 0, B_FULL, B_QUEUED };
static short *blk[NBLK];
static double blk_pts[NBLK];
static volatile int blk_state[NBLK];
static volatile int play_next = 0, voice_on = 0, starved = 0;
static int wr_blk = 0, wr_fill = 0;
static volatile double clk_pts = 0;
static volatile u64 clk_t0 = 0;
static volatile int clk_valid = 0;
static int volume = 220;
static volatile float alevel = 0;
static volatile int blk_peak[NBLK];

/* ------------------------------------------------------------------ estado */
static volatile int pstate = TP_IDLE, abort_req = 0, net_eof = 0;
static int cur_vmode = VM_AUTO;
static char perr[256] = "", pstatus[96] = "";
static char purl[8192];
static int vod_mode;static volatile int playlist_complete;
void tp_set_vod(int vod){vod_mode=vod;}
static lwp_t th_net = LWP_THREAD_NULL, th_dmx = LWP_THREAD_NULL, th_vid = LWP_THREAD_NULL;
static mutex_t mtx;
static volatile long long net_bytes = 0;
static volatile int has_audio = 0, has_video = 0, stream_w = 0, stream_h = 0;
static volatile int nframes = 0, ndropped = 0, nshown = 0;
volatile u32 tp_dec_us = 0;
static double fps_val = 0;
static int fps_cnt = 0; static u64 fps_t = 0;
static char variant_info[64] = "";
static int sd_stream = 1;
/* Twitch: limite de variante y refresco del token */
static int lim_h = 0;                 /* 0 = sin limite (comportamiento TV Gratis) */
static char refresh_api[200] = "";
static char fallback_url[4096] = "";   /* variante mas liviana (160p) para bajar si la red no da */
static int cur_h = 0, fallback_h = 0;
static volatile int n_refresh = 0, n_downgrade = 0;
static long last_code = 0;
static volatile int force_refresh = 0;
static volatile u64 last_pic_call = 0;  /* la UI esta mostrando video (no hay un menu abierto) */
void tp_force_refresh(void) { force_refresh = 1; }   /* pruebas: simula token vencido */
static volatile int force_reinit = 0;
void tp_force_reinit(void) { force_reinit = 1; }     /* pruebas: simula un init fMP4 distinto (anuncio) */

/* ------------------------------------------------------------------ cola de segmentos */
typedef struct seg { unsigned char *d; int n; struct seg *next; } seg_t;   /* n == 0: marca "nuevo init fMP4" */
static volatile int reopen_req = 0;     /* el demuxer tiene que reabrirse (cambio el init fMP4, p. ej. anuncio) */
#define REOPEN_SEGS 30                     /* ~60 s con segmentos de 2 s (Twitch) */
static int segs_since_init = 0;
static volatile int n_soft_reopen = 0;
static volatile int vgen = 0;           /* generacion de parametros de video (cambia al reabrir) */
static seg_t *sq_head = NULL, *sq_tail = NULL;
static volatile int sq_bytes = 0, sq_count = 0;
static seg_t *rd_seg = NULL; static int rd_pos = 0;

static void sq_push(unsigned char *d, int n) {
    seg_t *s = malloc(sizeof(seg_t));
    if (!s) { free(d); return; }
    s->d = d; s->n = n; s->next = NULL;
    LWP_MutexLock(mtx);
    if (sq_tail) sq_tail->next = s; else sq_head = s;
    sq_tail = s; sq_bytes += n; sq_count++;
    LWP_MutexUnlock(mtx);
}
static seg_t *sq_pop(void) {
    seg_t *s;
    LWP_MutexLock(mtx);
    s = sq_head;
    if (s) { sq_head = s->next; if (!sq_head) sq_tail = NULL; sq_bytes -= s->n; sq_count--; }
    LWP_MutexUnlock(mtx);
    return s;
}
static void sq_clear(void) {
    seg_t *s;
    while ((s = sq_pop())) { free(s->d); free(s); }
    if (rd_seg) { free(rd_seg->d); free(rd_seg); rd_seg = NULL; }
    rd_pos = 0;
}

/* ------------------------------------------------------------------ cola de paquetes de video */
#define VQ_MAX 360
static AVPacket *vq[VQ_MAX];
static volatile int vq_r = 0, vq_w = 0, vq_bytes = 0;
static int vq_needkey = 0;
static AVCodecParameters *vpar = NULL;
static AVRational vtb;
static int vq_count(void) { return (vq_w - vq_r + VQ_MAX) % VQ_MAX; }

/* ------------------------------------------------------------------ cuadros de salida */
#define NPIC 4
#define PIC_MAXW 640
#define PIC_MAXH 480
enum { P_FREE = 0, P_WRITING, P_READY, P_CUR };
static tv_pic_t pics[NPIC];
static volatile int pic_state[NPIC];
static int cur_pic = -1;
static double anchor_pts = 0; static u64 anchor_t = 0; static int anchor_ok = 0;

/* ================================================================== DSP */
static void voice_cb(s32 v) {
    int k = play_next;
    (void)v;
    if (!voice_on) return;
    if (blk_state[k] == B_FULL) {
        if (ASND_AddVoice(0, blk[k], BLKBYTES) == SND_OK) {
            int prev = (k + NBLK - 1) % NBLK, done = (k + NBLK - 2) % NBLK;
            blk_state[k] = B_QUEUED;
            if (blk_state[done] == B_QUEUED) blk_state[done] = B_FREE;
            clk_pts = blk_pts[prev]; clk_t0 = gettime(); clk_valid = 1;
            alevel = blk_peak[prev] / 32768.0f;
            play_next = (k + 1) % NBLK;
            starved = 0;
        }
    } else starved = 1;
}

static void voice_start(void) {
    int k = play_next;
    if (blk_state[k] != B_FULL) return;
    blk_state[k] = B_QUEUED;
    play_next = (k + 1) % NBLK;
    starved = 0;
    voice_on = 1;
    clk_pts = blk_pts[k]; clk_t0 = gettime(); clk_valid = 1;
    ASND_SetVoice(0, VOICE_STEREO_16BIT, 48000, 0, blk[k], BLKBYTES, volume, volume, voice_cb);
}

static void voice_reset(void) {
    int i;
    voice_on = 0;
    ASND_StopVoice(0);
    for (i = 0; i < NBLK; i++) blk_state[i] = B_FREE;
    play_next = 0; wr_blk = 0; wr_fill = 0; starved = 0; clk_valid = 0; alevel = 0;
}

static int full_blocks(void) {
    int i, n = 0;
    for (i = 0; i < NBLK; i++) if (blk_state[i] == B_FULL) n++;
    return n;
}

static double tp_clock(int *valid) {
    if (has_audio) {
        if (!clk_valid) { *valid = 0; return 0; }
        double e = ticks_to_microsecs(gettime() - clk_t0) / 1e6;
        if (e > BLKFR / 48000.0 * 1.5) e = BLKFR / 48000.0 * 1.5;
        *valid = 1;
        return clk_pts + e;
    }
    if (!anchor_ok) { *valid = 0; return 0; }
    *valid = 1;
    return anchor_pts + ticks_to_microsecs(gettime() - anchor_t) / 1e6;
}

void tp_init(void) {
    int i;
    ASND_Init();
    ASND_Pause(0);
    LWP_MutexInit(&mtx, false);
    for (i = 0; i < NBLK; i++) {
        blk[i] = memalign(32, BLKBYTES);
        memset(blk[i], 0, BLKBYTES);
    }
    for (i = 0; i < NPIC; i++) {
        pics[i].y = memalign(32, PIC_MAXW * PIC_MAXH);
        pics[i].u = memalign(32, PIC_MAXW * PIC_MAXH / 4);
        pics[i].v = memalign(32, PIC_MAXW * PIC_MAXH / 4);
        pic_state[i] = P_FREE;
    }
    av_log_set_level(AV_LOG_QUIET);
}

/* bloquea mientras el anillo esta lleno */
static void push_pcm(const short *p, int frames, double pts) {
    while (frames > 0 && !abort_req) {
        if (blk_state[wr_blk] != B_FREE) {
            if (!voice_on && full_blocks() >= START_BLK) voice_start();
            usleep(6000); continue;
        }
        if (wr_fill == 0) { blk_pts[wr_blk] = pts; blk_peak[wr_blk] = 0; }
        int take = BLKFR - wr_fill, i, pk = blk_peak[wr_blk];
        if (take > frames) take = frames;
        memcpy(blk[wr_blk] + wr_fill * 2, p, take * 4);
        for (i = 0; i < take * 2; i += 8) { int a = p[i] < 0 ? -p[i] : p[i]; if (a > pk) pk = a; }
        blk_peak[wr_blk] = pk;
        wr_fill += take; p += take * 2; frames -= take; pts += take / 48000.0;
        if (wr_fill == BLKFR) {
            DCFlushRange(blk[wr_blk], BLKBYTES);
            blk_state[wr_blk] = B_FULL;
            wr_blk = (wr_blk + 1) % NBLK;
            wr_fill = 0;
            int fb = full_blocks();
            if (!voice_on && fb >= START_BLK) { voice_start(); pstate = TP_PLAYING; }
            else if (voice_on && starved && fb >= 8) {
                /* el DSP se quedo sin audio: reanudar */
                int i2;
                ASND_StopVoice(0);
                for (i2 = 0; i2 < NBLK; i2++) if (blk_state[i2] == B_QUEUED) blk_state[i2] = B_FREE;
                while (blk_state[play_next] != B_FULL) play_next = (play_next + 1) % NBLK;
                voice_start();
                pstate = TP_PLAYING;
            }
        }
    }
}

/* ================================================================== red / HLS */
typedef struct { unsigned char *d; size_t n, cap, max; } mem_t;
static CURL *nc = NULL;
static size_t mem_wr(char *p, size_t s, size_t m, void *u) {
    mem_t *b = u; size_t n = s * m;
    if (abort_req) return 0;
    if (!b->cap) {
        /* reservar de una vez segun Content-Length (evita realloc y fragmentacion) */
        curl_off_t cl = -1;
        curl_easy_getinfo(nc, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0 && (size_t)cl + 1 <= b->max) {
            b->d = malloc((size_t)cl + 1 + 4096);
            if (b->d) b->cap = (size_t)cl + 1 + 4096;
        }
    }
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256 * 1024;
        while (nc < b->n + n + 1) nc *= 2;
        if (nc > b->max) nc = b->max;
        if (b->n + n + 1 > nc) return 0;
        unsigned char *nd = realloc(b->d, nc);
        if (!nd) return 0;
        b->d = nd; b->cap = nc;
    }
    memcpy(b->d + b->n, p, n); b->n += n; b->d[b->n] = 0;
    net_bytes += n;
    return n;
}
static int net_prog(void *c, curl_off_t a, curl_off_t b, curl_off_t d, curl_off_t e) {
    (void)c; (void)a; (void)b; (void)d; (void)e;
    return abort_req ? 1 : 0;
}

static char eff_url[4096];
static int fetch(const char *url, mem_t *m, size_t max) {
    char errbuf[CURL_ERROR_SIZE] = "";
    long code = 0; char *eu = NULL;
    m->d = NULL; m->n = 0; m->cap = 0; m->max = max;
    curl_easy_setopt(nc, CURLOPT_URL, url);
    curl_easy_setopt(nc, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(nc, CURLOPT_WRITEDATA, m);
    CURLcode r = curl_easy_perform(nc);
    curl_easy_getinfo(nc, CURLINFO_RESPONSE_CODE, &code);
    last_code = code;
    curl_easy_getinfo(nc, CURLINFO_EFFECTIVE_URL, &eu);
    snprintf(eff_url, sizeof(eff_url), "%s", eu ? eu : url);
    if (r != CURLE_OK || code < 200 || code >= 300 || !m->n) {
        if (!abort_req) {
            if (r != CURLE_OK) snprintf(perr, sizeof(perr), "Red: %s %s", curl_easy_strerror(r), errbuf);
            else if(vod_mode&&(code==401||code==403))snprintf(perr,sizeof(perr),"VOD restringido o solo suscriptores: Twitch denego acceso (%ld).",code);
            else snprintf(perr, sizeof(perr), "El servidor respondio HTTP %ld", code);
        }
        free(m->d); m->d = NULL; m->n = 0;
        return 0;
    }
    if (pstate != TP_ERROR) perr[0] = 0;
    return 1;
}

/* POST (solo para renovar el acceso de Twitch por GQL); deja el handle en GET al salir */
__attribute__((unused)) static int fetch_post(const char *url, const char *body, struct curl_slist *hl, mem_t *m, size_t max) {
    curl_easy_setopt(nc, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(nc, CURLOPT_POSTFIELDS, body);
    int ok = fetch(url, m, max);
    curl_easy_setopt(nc, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(nc, CURLOPT_HTTPGET, 1L);
    return ok;
}

static void resolve(const char *base, const char *rel, char *out, int sz) {
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) { snprintf(out, sz, "%s", rel); return; }
    if (rel[0] == '/' && rel[1] == '/') { const char *c = strstr(base, "//"); snprintf(out, sz, "%.*s%s", c ? (int)(c - base) : 0, base, rel); return; }
    if (rel[0] == '/') {
        const char *h = strstr(base, "//");
        const char *p = h ? strchr(h + 2, '/') : NULL;
        snprintf(out, sz, "%.*s%s", p ? (int)(p - base) : (int)strlen(base), base, rel);
        return;
    }
    const char *q = strchr(base, '?');
    int bl = q ? (int)(q - base) : (int)strlen(base);
    while (bl > 0 && base[bl - 1] != '/') bl--;
    snprintf(out, sz, "%.*s%s", bl, base, rel);
}

static char *next_line(char **p) {
    char *s = *p, *e;
    if (!s || !*s) return NULL;
    e = s + strcspn(s, "\r\n");
    if (*e) { *e++ = 0; while (*e == '\r' || *e == '\n') e++; }
    *p = e;
    return s;
}

/* elige la variante. Sin limite: la mas chica con video. Con limite (Twitch): la mas
   grande con alto <= lim_h (360p), y guarda la mas liviana (160p) como reserva.
   Nunca elige >= 480p cuando hay limite. */
static int pick_variant(char *txt, const char *base, char *out, int sz) {
    char *p = txt, *l, *pending = NULL;
    long best = -1; int found = 0;
    long fb_best = -1;
    char pend[512];
    fallback_url[0] = 0; fallback_h = 0; cur_h = 0;
    while ((l = next_line(&p))) {
        if (!strncmp(l, "#EXT-X-STREAM-INF", 17)) { snprintf(pend, sizeof(pend), "%s", l); pending = pend; continue; }
        if (pending && l[0] && l[0] != '#') {
            int w = 0, h = 0; long bw = 0; char *r;
            if ((r = strstr(pending, "RESOLUTION="))) sscanf(r + 11, "%dx%d", &w, &h);
            if ((r = strstr(pending, "BANDWIDTH="))) bw = atol(r + 10);
            int audio_only = (strstr(pending, "CODECS=") && !strstr(pending, "avc1") && !strstr(pending, "avc3") && !w);
            long score;
            if (lim_h > 0) {
                if (!h || h > lim_h || audio_only) { pending = NULL; continue; }   /* nunca 480p+ */
                if (fb_best < 0 || h < fb_best) { fb_best = h; resolve(base, l, fallback_url, sizeof(fallback_url)); fallback_h = h; }
                score = cur_vmode == VM_AUDIO ? bw : vod_mode ? h : (long)(10000 - h);        /* la mas alta <= limite */
            }
            else if (cur_vmode == VM_AUDIO) score = bw;
            else if (audio_only) score = 0x7FFFFFF0;
            else if (w) {
                score = (long)w * h;
                if (h < 200) score += 200000;   /* 144p/180p solo si no hay otra */
            } else score = 300000 + bw / 100;
            if (best < 0 || score < best) {
                best = score; found = 1; cur_h = h;
                resolve(base, l, out, sz);
                if (w) snprintf(variant_info, sizeof(variant_info), "%dp %ld kbps", h, bw / 1000);
                else snprintf(variant_info, sizeof(variant_info), "%ld kbps", bw / 1000);
            }
            pending = NULL;
        }
    }
    if (fallback_h >= cur_h) fallback_url[0] = 0;
    return found;
}

/* baja la lista maestra (y sub-maestras) y deja en url la variante elegida */
static int open_master(const char *murl, char *url, int usz) {
    mem_t m;
    int level;
    snprintf(url, usz, "%s", murl);
    for (level = 0; level < 3 && !abort_req; level++) {
        if (!fetch(url, &m, 2 * 1024 * 1024)) return 0;
        if (!strstr((char *)m.d, "#EXTM3U")) { snprintf(perr, sizeof(perr), "La direccion no es una lista HLS (.m3u8)."); free(m.d); return 0; }
        if (!strstr((char *)m.d, "#EXT-X-STREAM-INF")) { free(m.d); return 1; }
        char base[4096]; snprintf(base, sizeof(base), "%s", eff_url);
        char nu[4096];
        if (!pick_variant((char *)m.d, base, nu, sizeof(nu))) {
            free(m.d);
            snprintf(perr, sizeof(perr), lim_h ? "El canal no ofrece 360p/160p (la Wii no puede con 480p o mas)." : "Lista maestra sin variantes.");
            return 0;
        }
        free(m.d);
        snprintf(url, usz, "%s", nu);
    }
    return 1;
}

/* Twitch: el token de usher vence. Pide uno nuevo al servidor companion (/wii/token/<canal>
   o al companion, que responde con la misma forma) y vuelve a abrir la lista maestra.
   Una sola peticion, solo si el video fallo. */
int tw_parse_token(const char *json, kstream_t *info, char *url, int usz);
static int refresh_token(char *url, int usz) {
    mem_t m;
    char murl[4096];
    kstream_t ks;
    if (!refresh_api[0] || n_refresh >= 20 || abort_req) return 0;
    n_refresh++;
    snprintf(pstatus, sizeof(pstatus), "Renovando acceso...");
    if (!fetch(refresh_api, &m, 512 * 1024)) return 0;
    memset(&ks, 0, sizeof(ks));
    int live = tw_parse_token((char *)m.d, &ks, murl, sizeof(murl));
    free(m.d);
    if (live != 1 || strncmp(murl, "https://", 8)) { snprintf(perr, sizeof(perr), "La transmision termino (el canal ya no esta en vivo)."); return 0; }
    snprintf(purl, sizeof(purl), "%s", murl);
    return open_master(purl, url, usz);
}

static void *net_thread(void *arg) {
    static char url[8192], segu[4096], cur_map[4096], mapu[4096];
    unsigned char *last_init = NULL; int last_init_n = 0;
    mem_t m;
    int fails = 0, slow = 0, cpu_d0 = 0, cpu_f0 = 0;
    cur_map[0] = 0; segs_since_init = 0;
    u64 cpu_t = 0;
    long long next_seq = -1;
    (void)arg;
    nc = curl_easy_init();
    if (!nc) { snprintf(perr, sizeof(perr), "No se pudo iniciar libcurl."); pstate = TP_ERROR; net_eof = 1; return NULL; }
    {
        static char eb[CURL_ERROR_SIZE];
        sh_curl_common(nc, eb);
    }
    curl_easy_setopt(nc, CURLOPT_WRITEFUNCTION, mem_wr);
    curl_easy_setopt(nc, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(nc, CURLOPT_XFERINFOFUNCTION, net_prog);
    curl_easy_setopt(nc, CURLOPT_LOW_SPEED_LIMIT, 2000L);
    curl_easy_setopt(nc, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(nc, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(nc, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(nc, CURLOPT_BUFFERSIZE, 16L * 1024);   /* recv() de IOS falla con bloques grandes por HTTP plano ("Not enough space") */
    curl_easy_setopt(nc, CURLOPT_ACCEPT_ENCODING, "");

    snprintf(pstatus, sizeof(pstatus), "Conectando...");
    if (!open_master(purl, url, sizeof(url))) {
        if (abort_req || !(last_code == 403 || last_code == 401) || !refresh_token(url, sizeof(url))) goto fail;
    }
    {
        int w = 0, h = 0; char *x = strchr(variant_info, 'x');
        if (x) { w = atoi(variant_info); h = atoi(x + 1); }
        sd_stream = !w || (w * h <= 640 * 368);
        if (lim_h) sd_stream = 1;
    }
    snprintf(pstatus, sizeof(pstatus), "Cargando...");
    while (!abort_req) {
        if (force_reinit && cur_map[0]) {
            force_reinit = 0; cur_map[0] = 0;
            free(last_init); last_init = malloc(1); last_init_n = -1;   /* el proximo init se toma como distinto */
        }
        if (force_refresh && refresh_api[0]) {
            force_refresh = 0;
            if (!refresh_token(url, sizeof(url))) goto fail;
            snprintf(pstatus, sizeof(pstatus), "Cargando...");
        }
        if (!fetch(url, &m, 2 * 1024 * 1024)) {
            /* 403/401/404 en la lista: casi siempre token vencido -> renovar */
            if (!abort_req && refresh_api[0] && (last_code == 403 || last_code == 401 || last_code == 404 || fails >= 2)) {
                if (refresh_token(url, sizeof(url))) { fails = 0; snprintf(pstatus, sizeof(pstatus), "Cargando..."); continue; }
                goto fail;
            }
            if (++fails >= 6) goto fail;
            usleep(1000 * 1000); continue;
        }
        static char base[4096]; snprintf(base, sizeof(base), "%s", eff_url);
        char *p = (char *)m.d, *l, *map_now = NULL;
        int td = 6, endlist = 0, nseg = 0, i;
        long long seq0 = 0;
        static char *seg_uri[16384], *seg_map[16384];
        while ((l = next_line(&p))) {
            if (!strncmp(l, "#EXT-X-TARGETDURATION:", 22)) td = atoi(l + 22);
            else if (!strncmp(l, "#EXT-X-MEDIA-SEQUENCE:", 22)) seq0 = atoll(l + 22);
            else if (!strncmp(l, "#EXT-X-ENDLIST", 14)) endlist = 1;
            else if (!strncmp(l, "#EXT-X-KEY:", 11) && !strstr(l, "METHOD=NONE")) {
                snprintf(perr, sizeof(perr), "Transmision cifrada (DRM/AES): no compatible.");
                free(m.d); goto fail;
            } else if (!strncmp(l, "#EXT-X-MAP:", 11)) {
                /* fMP4 (Twitch): segmento de inicializacion (moov). Sin BYTERANGE. */
                char *u = strstr(l, "URI=\"");
                if (!u || strstr(l, "BYTERANGE")) { snprintf(perr, sizeof(perr), "Formato fMP4 no compatible (EXT-X-MAP con BYTERANGE)."); free(m.d); goto fail; }
                u += 5; u[strcspn(u, "\"")] = 0;
                map_now = u;
            } else if (l[0] && l[0] != '#') {
                if(nseg>=16384){snprintf(perr,sizeof(perr),"Lista HLS demasiado grande para Wii");free(m.d);goto fail;}
                seg_map[nseg]=map_now;seg_uri[nseg++]=l;
            }
        }
        if (td < 1) td = 1;
        if (td > 20) td = 20;
        fails = 0;
        if (!nseg) { free(m.d); usleep(1000 * 1000); continue; }
        if (next_seq < 0 || next_seq < seq0 || next_seq > seq0 + nseg + 2) {
            int back = td >= 8 ? 2 : 3;
            next_seq = (vod_mode||endlist)?seq0:seq0+(nseg>back?nseg-back:0);
        }
        int got = 0;
        for (i = (int)(next_seq - seq0); i < nseg && !abort_req; i++) {
            while (!abort_req && (sq_count >= 3 || sq_bytes > 8 * 1024 * 1024)) usleep(20000);
            if (abort_req) break;
            if (seg_map[i]) {
                resolve(base, seg_map[i], mapu, sizeof(mapu));
                if (strcmp(mapu, cur_map)) {
                    mem_t im;
                    if (!fetch(mapu, &im, 1024 * 1024) && (abort_req || !fetch(mapu, &im, 1024 * 1024))) { free(m.d); goto fail; }
                    snprintf(cur_map, sizeof(cur_map), "%s", mapu);
                    if (last_init && last_init_n == (int)im.n && !memcmp(last_init, im.d, im.n)) {
                        free(im.d);                          /* mismo init con otra URL (token renovado): seguir igual */
                    } else {
                        if (last_init) sq_push(NULL, 0);     /* init distinto (anuncio o cambio de calidad): reabrir demuxer */
                        free(last_init);
                        last_init = malloc(im.n); last_init_n = (int)im.n;
                        if (last_init) memcpy(last_init, im.d, im.n);
                        sq_push(im.d, (int)im.n);
                        segs_since_init = 0;
                    }
                }
            }
            /* rc9: en vivo fMP4 el demuxer mov agrega una entrada de indice por muestra (y un
               item de frag_index por moof) que nunca se liberan; sus arrays crecen con
               av_fast_realloc de a ~6% y dejan huecos en el heap (MEM2 bajaba 0,4-1 MB/min).
               Cada REOPEN_SEGS segmentos se vuelve a mandar el mismo init para que el demuxer
               se reabra con el indice vacio (los decodificadores se conservan: ver dmx_thread). */
            if (seg_map[i] && last_init && last_init_n > 0 && !vod_mode) {
                if (++segs_since_init >= REOPEN_SEGS) {
                    unsigned char *ic = malloc(last_init_n);
                    if (ic) { memcpy(ic, last_init, last_init_n); sq_push(NULL, 0); sq_push(ic, last_init_n); n_soft_reopen++; }
                    segs_since_init = 0;
                }
            }
            resolve(base, seg_uri[i], segu, sizeof(segu));
            mem_t s;
            u64 ts0 = gettime();
            if (fetch(segu, &s, 24 * 1024 * 1024) || (!abort_req && fetch(segu, &s, 24 * 1024 * 1024))) {
                /* red lenta: si 3 segmentos seguidos tardan mas del 85% de su duracion, bajar a 160p */
                u32 ms = ticks_to_millisecs(gettime() - ts0);
                if (fallback_url[0] && ms > (u32)(td * 850)) slow++; else slow = 0;
                if (!seg_map[i] && s.n >= 188 && s.d[0] != 0x47) {
                    /* buscar el primer byte de sincronia TS */
                    size_t o = 0;
                    while (o + 188 < s.n && !(s.d[o] == 0x47 && s.d[o + 188] == 0x47)) o++;
                    if (o + 188 >= s.n) { free(s.d); snprintf(perr, sizeof(perr), "El segmento no es MPEG-TS."); free(m.d); goto fail; }
                    memmove(s.d, s.d + o, s.n - o); s.n -= o;
                }
                sq_push(s.d, (int)s.n);
                got++;
            }
            next_seq = seq0 + i + 1;
            /* CPU: si en 12 s se descarta mas del 40% de los cuadros (la Wii no llega
               a decodificar 360p a tiempo), tambien se baja a 160p */
            if (fallback_url[0] && has_video && pstate == TP_PLAYING) {
                u64 now = gettime();
                if (ticks_to_millisecs(now - last_pic_call) > 300) cpu_t = 0;   /* menu abierto: no medir */
                else if (!cpu_t) { cpu_t = now; cpu_d0 = ndropped; cpu_f0 = nframes; }
                else if (ticks_to_millisecs(now - cpu_t) >= 12000) {
                    int dd = ndropped - cpu_d0, ff = nframes - cpu_f0;
                    if (dd > 100 && dd * 100 > ff * 40) slow = 3;
                    cpu_t = now; cpu_d0 = ndropped; cpu_f0 = nframes;
                }
            }
            if (slow >= 3 && fallback_url[0]) {
                snprintf(url, sizeof(url), "%s", fallback_url);
                snprintf(variant_info, sizeof(variant_info), "%dp (auto, liviano)", fallback_h);
                cur_h = fallback_h; fallback_url[0] = 0; slow = 0; n_downgrade++;
                break;
            }
            /* la URL del segmento cambio la URL efectiva: recuperar la de la lista */
        }
        free(m.d);
        if (endlist && next_seq >= seq0 + nseg) { playlist_complete=1;net_eof = 1; break; }
        if (!got) {
            int k, wait = td * 500;        /* media duracion de segmento */
            if (wait < 1000) wait = 1000;
            for (k = 0; k < wait / 50 && !abort_req; k++) usleep(50000);
        }
    }
    net_eof = 1;
    free(last_init);
    curl_easy_cleanup(nc); nc = NULL;
    return NULL;
fail:
    if (!abort_req) { pstate = TP_ERROR; if (!perr[0]) snprintf(perr, sizeof(perr), "No se pudo abrir la transmision."); }
    net_eof = 1;
    free(last_init);
    curl_easy_cleanup(nc); nc = NULL;
    return NULL;
}

/* ================================================================== demux + audio */
static int io_read(void *o, uint8_t *b, int sz) {
    (void)o;
    /* despues de la marca de init nuevo, EOF "pegajoso": el demuxer viejo no debe
       comerse el init ni los fragmentos que siguen (son para el demuxer nuevo) */
    if (reopen_req) return AVERROR_EOF;
    while (!abort_req) {
        if (rd_seg && rd_pos < rd_seg->n) {
            int k = rd_seg->n - rd_pos;
            if (k > sz) k = sz;
            memcpy(b, rd_seg->d + rd_pos, k);
            rd_pos += k;
            return k;
        }
        if (rd_seg) { free(rd_seg->d); free(rd_seg); rd_seg = NULL; }
        rd_seg = sq_pop(); rd_pos = 0;
        if (rd_seg && rd_seg->n == 0) { free(rd_seg); rd_seg = NULL; reopen_req = 1; return AVERROR_EOF; }
        if (rd_seg) continue;
        if (net_eof) return AVERROR_EOF;
        usleep(5000);
    }
    return AVERROR_EXIT;
}
static int io_interrupt(void *o) { (void)o; return abort_req; }

static void vq_push(AVPacket *pk) {
    int key = (pk->flags & AV_PKT_FLAG_KEY) != 0;
    if (vq_needkey && !key) { ndropped++; return; }
    if (vq_count() >= VQ_MAX - 1 || vq_bytes > 5 * 1024 * 1024) { vq_needkey = 1; ndropped++; return; }
    vq_needkey = 0;
    AVPacket *c = av_packet_clone(pk);
    if (!c) return;
    c->pos = vgen;                       /* generacion de vpar con la que hay que decodificarlo */
    vq[vq_w] = c;
    LWP_MutexLock(mtx); vq_bytes += c->size; LWP_MutexUnlock(mtx);
    vq_w = (vq_w + 1) % VQ_MAX;
}

/* espera el primer segmento y mira si es MPEG-TS (0x47) o fMP4 (ftyp/moov/styp) */
static const char *detect_format(void) {
    while (!abort_req) {
        seg_t *h;
        LWP_MutexLock(mtx);
        h = (rd_seg && rd_pos < rd_seg->n) ? rd_seg : sq_head;
        int n = h ? h->n : -1;
        unsigned char b[8] = { 0 };
        if (h && n >= 8) memcpy(b, h->d + (h == rd_seg ? rd_pos : 0), 8);
        LWP_MutexUnlock(mtx);
        if (n >= 8) {
            if (!memcmp(b + 4, "ftyp", 4) || !memcmp(b + 4, "moov", 4) || !memcmp(b + 4, "styp", 4)) return "mov";
            return "mpegts";
        }
        if (n == 0) { seg_t *x = sq_pop(); if (x) { free(x->d); free(x); } continue; }   /* marca suelta */
        if (net_eof) return NULL;
        usleep(5000);
    }
    return NULL;
}

static void *dmx_thread(void *arg) {
    AVFormatContext *fc = NULL;
    AVIOContext *io = NULL;
    AVCodecContext *ac = NULL;
    SwrContext *swr = NULL;
    AVPacket *pk = NULL; AVFrame *fr = NULL;
    int ai = -1, vi = -1, opened = 0;
    static short outbuf[8192 * 2];
    double a_next = -1;
    u64 t_start = gettime();
    (void)arg;

    pk = av_packet_alloc(); fr = av_frame_alloc();
    while (!abort_req) {
        /* (re)abrir el demuxer: al empezar y cada vez que llega un init fMP4 nuevo */
        const char *fmt = detect_format();
        if (!fmt) break;
        reopen_req = 0;
        unsigned char *ib = av_malloc(32768);
        io = avio_alloc_context(ib, 32768, 0, NULL, io_read, NULL, NULL);
        fc = avformat_alloc_context();
        if (!io || !fc || !pk || !fr) { snprintf(perr, sizeof(perr), "Sin memoria."); pstate = TP_ERROR; goto out; }
        fc->pb = io;
        fc->interrupt_callback.callback = io_interrupt;
        fc->probesize = 512 * 1024;
        fc->max_analyze_duration = 2 * AV_TIME_BASE;
        if (avformat_open_input(&fc, "", av_find_input_format(fmt), NULL) < 0) {
            if (!abort_req && pstate != TP_ERROR) { snprintf(perr, sizeof(perr), "No se pudo leer el video (%s).", fmt[0] == 'm' && fmt[1] == 'o' ? "fMP4" : "MPEG-TS"); pstate = TP_ERROR; }
            goto out;
        }
        ai = -1; vi = -1;
        /* reapertura: el audio se conserva si los parametros son iguales (ver abajo) */
        opened++; (void)opened;
        if (pstate != TP_ERROR && pstate != TP_PLAYING) { snprintf(pstatus, sizeof(pstatus), "Llenando buffer..."); pstate = TP_BUFFERING; }
        while (!abort_req) {
            int r = av_read_frame(fc, pk);
            if (r < 0) break;
            AVStream *st = (pk->stream_index < (int)fc->nb_streams) ? fc->streams[pk->stream_index] : NULL;
            if (!st) { av_packet_unref(pk); continue; }
            int ct = st->codecpar->codec_type;
            if (ai < 0 && ct == AVMEDIA_TYPE_AUDIO && ac && ac->codec_id == st->codecpar->codec_id && ac->sample_rate == st->codecpar->sample_rate &&
                ac->ch_layout.nb_channels == st->codecpar->ch_layout.nb_channels && ac->extradata_size == st->codecpar->extradata_size &&
                (!ac->extradata_size || !memcmp(ac->extradata, st->codecpar->extradata, ac->extradata_size))) {
                ai = pk->stream_index;   /* mismo audio tras reabrir: seguir con el mismo decoder */
            } else if (ai < 0 && ct == AVMEDIA_TYPE_AUDIO && ac) {
                avcodec_free_context(&ac); swr_free(&swr);   /* audio distinto (anuncio, otra calidad) */
            }
            if (ai < 0 && !ac && ct == AVMEDIA_TYPE_AUDIO && (st->codecpar->codec_id == AV_CODEC_ID_AAC || st->codecpar->codec_id == AV_CODEC_ID_MP3 || st->codecpar->codec_id == AV_CODEC_ID_AAC_LATM)) {
                const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
                ac = c ? avcodec_alloc_context3(c) : NULL;
                if (ac && avcodec_parameters_to_context(ac, st->codecpar) >= 0 && avcodec_open2(ac, c, NULL) >= 0) { ai = pk->stream_index; has_audio = 1; }
                else avcodec_free_context(&ac);
            }
            if (vi < 0 && ct == AVMEDIA_TYPE_VIDEO && st->codecpar->codec_id == AV_CODEC_ID_H264 && cur_vmode != VM_AUDIO) {
                const AVCodecParameters *cp = st->codecpar;
                LWP_MutexLock(mtx);
                int same = vpar && vpar->codec_id == cp->codec_id && vpar->width == cp->width && vpar->height == cp->height &&
                           vpar->extradata_size == cp->extradata_size && (!cp->extradata_size || !memcmp(vpar->extradata, cp->extradata, cp->extradata_size)) &&
                           vtb.num == st->time_base.num && vtb.den == st->time_base.den;
                LWP_MutexUnlock(mtx);
                if (!same) {   /* rc9: mismo SPS/PPS tras reabrir -> mismo decoder (sin recrearlo) */
                    AVCodecParameters *np = avcodec_parameters_alloc();
                    avcodec_parameters_copy(np, cp);
                    LWP_MutexLock(mtx);
                    AVCodecParameters *old = vpar;
                    vpar = np; vtb = st->time_base; vgen++;
                    LWP_MutexUnlock(mtx);
                    if (old) avcodec_parameters_free(&old);
                }
                vi = pk->stream_index; has_video = 1;
            }
            if (pk->stream_index == ai) {
                if (avcodec_send_packet(ac, pk) >= 0) {
                    while (avcodec_receive_frame(ac, fr) == 0) {
                        double pts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * av_q2d(st->time_base) : a_next;
                        if (!swr) {
                            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &fr->ch_layout, fr->format, fr->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
                                snprintf(perr, sizeof(perr), "No se pudo convertir el audio."); swr_free(&swr); break;
                            }
                        }
                        uint8_t *o[1] = { (uint8_t *)outbuf };
                        int n = swr_convert(swr, o, 8192, (const uint8_t **)fr->extended_data, fr->nb_samples);
                        if (n > 0) {
                            push_pcm(outbuf, n, pts);
                            a_next = pts + n / 48000.0;
                        }
                    }
                }
            } else if (pk->stream_index == vi) {
                vq_push(pk);
            } else if (ai < 0 && vi >= 0 && ticks_to_millisecs(gettime() - t_start) > 4000 && pstate == TP_BUFFERING) {
                pstate = TP_PLAYING;   /* sin audio */
            }
            if (!has_audio && has_video && pstate == TP_BUFFERING && ticks_to_millisecs(gettime() - t_start) > 4000) pstate = TP_PLAYING;
            av_packet_unref(pk);
        }
        avformat_close_input(&fc);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
        if (!reopen_req || abort_req) break;
    }
    if (!abort_req && pstate != TP_ERROR) {
        if(vod_mode&&playlist_complete&&net_eof) {
            u64 end=gettime();while(!abort_req&&(full_blocks()>0||vq_count()>0)&&ticks_to_millisecs(gettime()-end)<5000)usleep(20000);
            if(!abort_req){perr[0]=0;pstate=TP_ENDED;}
        } else {if(!perr[0])snprintf(perr,sizeof(perr),"La transmision termino o se corto.");pstate=TP_ERROR;}
    }
out:
    av_frame_free(&fr); av_packet_free(&pk);
    avcodec_free_context(&ac);
    swr_free(&swr);
    if (fc) avformat_close_input(&fc);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    return NULL;
}

/* ================================================================== video */
static void store_frame(AVFrame *f, double pts) {
    int k = -1, i, j;
    while (!abort_req) {
        for (i = 0; i < NPIC; i++) if (pic_state[i] == P_FREE) { k = i; break; }
        if (k >= 0) break;
        usleep(3000);
    }
    if (k < 0) return;
    pic_state[k] = P_WRITING;
    tv_pic_t *p = &pics[k];
    int fw = f->width, fh = f->height, step = 1;
    while (fw / step > PIC_MAXW || fh / step > PIC_MAXH) step++;
    int dw = (fw / step) & ~1, dh = (fh / step) & ~1;
    p->aspect = (float)fw / fh;
    if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
        p->aspect *= (float)f->sample_aspect_ratio.num / f->sample_aspect_ratio.den;
    p->w = dw; p->h = dh; p->sw = fw; p->sh = fh; p->pts = pts;
    for (j = 0; j < dh; j++) {
        const u8 *s = f->data[0] + j * step * f->linesize[0];
        u8 *d = p->y + j * dw;
        if (step == 1) memcpy(d, s, dw);
        else for (i = 0; i < dw; i++) d[i] = s[i * step];
    }
    for (j = 0; j < dh / 2; j++) {
        const u8 *su = f->data[1] + j * step * f->linesize[1], *sv = f->data[2] + j * step * f->linesize[2];
        u8 *du = p->u + j * (dw / 2), *dv = p->v + j * (dw / 2);
        if (step == 1) { memcpy(du, su, dw / 2); memcpy(dv, sv, dw / 2); }
        else for (i = 0; i < dw / 2; i++) { du[i] = su[i * step]; dv[i] = sv[i * step]; }
    }
    pic_state[k] = P_READY;
}

static void *vid_thread(void *arg) {
    AVCodecContext *vc = NULL;
    AVFrame *fr = av_frame_alloc();
    u64 last_store = 0;
    long long dec_gen = -1;
    (void)arg;
    while (!abort_req) {
        if (vq_r == vq_w) { usleep(4000); continue; }
        AVPacket *pk = vq[vq_r];
        vq[vq_r] = NULL;
        LWP_MutexLock(mtx); vq_bytes -= pk->size; LWP_MutexUnlock(mtx);
        vq_r = (vq_r + 1) % VQ_MAX;
        if (vc && pk->pos != dec_gen) avcodec_free_context(&vc);   /* init fMP4 nuevo: otro SPS/PPS */
        if (!vc) {
            const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
            vc = avcodec_alloc_context3(c);
            LWP_MutexLock(mtx);
            if (vpar) avcodec_parameters_to_context(vc, vpar);
            dec_gen = vgen;
            LWP_MutexUnlock(mtx);
            vc->thread_count = 1;
            vc->flags2 |= AV_CODEC_FLAG2_FAST;
            if (avcodec_open2(vc, c, NULL) < 0) { avcodec_free_context(&vc); av_packet_free(&pk); has_video = 0; break; }
        }
        int valid = 0;
        double clock = tp_clock(&valid);
        double ppts = pk->pts != AV_NOPTS_VALUE ? pk->pts * av_q2d(vtb) : (pk->dts != AV_NOPTS_VALUE ? pk->dts * av_q2d(vtb) : -1);
        double lag = (valid && ppts >= 0) ? clock - ppts : 0;
        int key = (pk->flags & AV_PKT_FLAG_KEY) != 0;
        if (lag > 30 || lag < -30) lag = 0;      /* discontinuidad: no descartar */
        if (vc->width * vc->height > 640 * 368) vc->skip_loop_filter = AVDISCARD_ALL;
        if (lag > 1.2 && !key) { av_packet_free(&pk); ndropped++; continue; }
        vc->skip_frame = lag > 0.3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        u64 td0 = gettime();
        int sent = avcodec_send_packet(vc, pk) >= 0;
        tp_dec_us += ticks_to_microsecs(gettime() - td0);
        if (sent) {
            while (1) {
                td0 = gettime();
                int rr = avcodec_receive_frame(vc, fr);
                tp_dec_us += ticks_to_microsecs(gettime() - td0);
                if (rr != 0) break;
                double fpts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * av_q2d(vtb) : ppts;
                nframes++;
                stream_w = fr->width; stream_h = fr->height;
                clock = tp_clock(&valid);
                if (valid && fpts < clock - 0.25 && fpts > clock - 30 && ticks_to_millisecs(gettime() - last_store) < 700) { ndropped++; continue; }
                if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P) continue;
                store_frame(fr, fpts);
                last_store = gettime();
            }
        }
        av_packet_free(&pk);
    }
    avcodec_free_context(&vc);
    av_frame_free(&fr);
    return NULL;
}

/* ================================================================== hilo principal */
const tv_pic_t *tp_picture(void) {
    int i, valid = 0, best = -1, minp = -1;
    double clock = tp_clock(&valid);
    last_pic_call = gettime();
    if (pstate == TP_IDLE) return NULL;
    for (i = 0; i < NPIC; i++) {
        if (pic_state[i] != P_READY) continue;
        if (minp < 0 || pics[i].pts < pics[minp].pts) minp = i;
        if (valid && pics[i].pts <= clock + 0.02 && (best < 0 || pics[i].pts > pics[best].pts)) best = i;
    }
    if (best < 0 && minp >= 0) {
        if (cur_pic < 0) best = minp;                                    /* primer cuadro: mostrarlo ya */
        else if (valid && pics[minp].pts > clock + 2.5) best = minp;     /* salto de tiempo */
        else if (!has_audio && !anchor_ok) best = minp;
    }
    if (best >= 0) {
        if (!has_audio && (!anchor_ok || fabs(pics[best].pts - clock) > 2.5)) { anchor_pts = pics[best].pts; anchor_t = gettime(); anchor_ok = 1; }
        for (i = 0; i < NPIC; i++)
            if (i != best && pic_state[i] == P_READY && pics[i].pts <= pics[best].pts) pic_state[i] = P_FREE;
        if (cur_pic >= 0 && cur_pic != best) pic_state[cur_pic] = P_FREE;
        pic_state[best] = P_CUR;
        cur_pic = best;
        nshown++; fps_cnt++;
    }
    u64 now = gettime();
    if (!fps_t) fps_t = now;
    if (ticks_to_millisecs(now - fps_t) >= 1000) { fps_val = fps_cnt * 1000.0 / ticks_to_millisecs(now - fps_t); fps_cnt = 0; fps_t = now; }
    return cur_pic >= 0 ? &pics[cur_pic] : NULL;
}

void tp_stop(void) {
    int i;
    abort_req = 1;
    if (th_vid != LWP_THREAD_NULL) { LWP_JoinThread(th_vid, NULL); th_vid = LWP_THREAD_NULL; }
    if (th_dmx != LWP_THREAD_NULL) { LWP_JoinThread(th_dmx, NULL); th_dmx = LWP_THREAD_NULL; }
    if (th_net != LWP_THREAD_NULL) { LWP_JoinThread(th_net, NULL); th_net = LWP_THREAD_NULL; }
    voice_reset();
    sq_clear();
    while (vq_r != vq_w) { av_packet_free(&vq[vq_r]); vq_r = (vq_r + 1) % VQ_MAX; }
    vq_r = vq_w = 0; vq_needkey = 0; vq_bytes = 0; reopen_req = 0;
    if (vpar) avcodec_parameters_free(&vpar);
    for (i = 0; i < NPIC; i++) pic_state[i] = P_FREE;
    cur_pic = -1; anchor_ok = 0;
    pstate = TP_IDLE; net_eof = 0;playlist_complete=0; abort_req = 0;
    has_audio = has_video = 0; stream_w = stream_h = 0;
    nframes = ndropped = nshown = 0; fps_val = 0; fps_cnt = 0; fps_t = 0;
    net_bytes = 0; variant_info[0] = 0; pstatus[0] = 0;
}

void tp_set_limits(int max_h, const char *api) {
    lim_h = max_h;
    snprintf(refresh_api, sizeof(refresh_api), "%s", api ? api : "");
}

int tp_play(const char *url, int vmode) {
    tp_stop();
    perr[0] = 0;
    cur_vmode = vmode;
    n_refresh = 0; n_downgrade = 0;
    snprintf(purl, sizeof(purl), "%s", url);
    pstate = TP_CONNECTING;
    if (!sh_net_ready()) { snprintf(perr, sizeof(perr), "La red no esta disponible."); pstate = TP_ERROR; return 0; }
    if (LWP_CreateThread(&th_net, net_thread, NULL, NULL, 128 * 1024, 72) < 0 ||
        LWP_CreateThread(&th_dmx, dmx_thread, NULL, NULL, 256 * 1024, 70) < 0 ||
        LWP_CreateThread(&th_vid, vid_thread, NULL, NULL, 256 * 1024, 40) < 0) {
        snprintf(perr, sizeof(perr), "No se pudieron crear los hilos."); pstate = TP_ERROR; return 0;
    }
    return 1;
}

int tp_state(void) { return pstate; }
const char *tp_error(void) { return perr; }
const char *tp_status(void) {
    static char s[96];
    switch (pstate) {
    case TP_IDLE: return "Detenido";
    case TP_CONNECTING: return pstatus[0] ? pstatus : "Conectando...";
    case TP_BUFFERING: {
        int fb = full_blocks();
        snprintf(s, sizeof(s), "Cargando %d%%", has_audio ? (fb * 100 / START_BLK > 99 ? 99 : fb * 100 / START_BLK) : 50);
        return s; }
    case TP_PLAYING: return vod_mode?"Reproduciendo VOD":"En vivo";
    case TP_ENDED:return "Fin del VOD";
    case TP_ERROR: return "Error";
    }
    return "";
}
void tp_set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    volume = v;
    if (voice_on) ASND_ChangeVolumeVoice(0, v, v);
}
int tp_volume(void) { return volume; }
int tp_has_video(void) { return has_video; }
int tp_has_audio(void) { return has_audio; }
int tp_vmode(void) { return cur_vmode; }
const char *tp_mode_text(void) {
    if (cur_vmode == VM_AUDIO) return "Solo audio";
    if (pstate == TP_PLAYING && !has_video) return "Solo audio (sin video)";
    if (lim_h) return cur_h && cur_h < 300 ? "Video 160p (liviano)" : "Video 360p";
    if (stream_w * stream_h > 640 * 368 || !sd_stream) return "Video HD reducido";
    return "Video completo";
}
float tp_audio_level(void) { return voice_on ? alevel : 0; }
double tp_fps(void) { return fps_val; }
int tp_frames(void) { return nframes; }
int tp_dropped(void) { return ndropped; }
long long tp_bytes(void) { return net_bytes; }
void tp_stats(char *o, int sz) {
    snprintf(o, sz, "%s%s%dx%d  %.0f fps  buf %.1fs  red %lld KB  desc %d%s",
             variant_info, variant_info[0] ? " | " : "", stream_w, stream_h, fps_val,
             full_blocks() * BLKFR / 48000.0, net_bytes >> 10, ndropped, n_refresh ? "  token renovado" : "");
#ifdef AUTOTEST
    { size_t l = strlen(o); if ((int)l < sz) snprintf(o + l, sz - l, "  reab %d", n_soft_reopen); }
#endif
}
