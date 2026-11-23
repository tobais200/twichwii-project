/* thumbs.c - imagenes asincronas (rc13 KickWii / rc7 TwichWii).
 *
 * rc13: antes habia UN hilo y cada imagen abria una conexion HTTPS nueva
 * (DNS + TCP + handshake TLS con mbedTLS en el CPU de la Wii + parseo del
 * paquete de CA). Ahora:
 *  - 2 hilos de descarga, cada uno con su handle de curl PERSISTENTE:
 *    keep-alive, la conexion a images.kick.com / static-cdn.jtvnw.net se
 *    reutiliza y el handshake se hace una sola vez por host y por hilo;
 *  - los banners grandes se piden a la resolucion de pantalla cuando el CDN
 *    publica ese tamano (Twitch offline 1920x1080 -> 320x180, avatar Kick
 *    fullsize -> medium), con vuelta a la URL original si falla;
 *  - JPEG se decodifica con escala DCT 1/2..1/8 (un banner Kick de 2560x1440
 *    y 3,4 MB ya no se rechaza ni se decodifica entero);
 *  - cache en disco (SD o, si no hay, USB; nunca toca un dispositivo ausente)
 *    para avatares, banners, categorias y emotes: 7 dias. Las miniaturas en
 *    vivo no se guardan (cambian cada pocos minutos);
 *  - emotes del chat (cola propia, 1 descarga a la vez, tambien durante el
 *    video).  */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include <curl/curl.h>
#include "tv.h"
#include "image_decode.h"
#include "clock_sync.h"
#ifndef BENCH_WAIT
#define BENCH_WAIT 30
#endif

#define NSLOT 64
#define NWORK 3
#define MAXIMG (4 * 1024 * 1024)
#define DISK_TTL (7 * 86400)
#define EMOTE_H 32
#define EMOTE_MAX (384 * 1024)
enum { S_FREE = 0, S_WANT, S_LOADING, S_READY, S_FAIL };
typedef struct {
    char path[768];
    int state, emote;
    u64 retry; unsigned failures;
    u32 used, seq;
    kimg_t img;
} slot_t;
typedef struct { CURL *c; unsigned char *buf; int cap, n, lim; volatile int active; int id; char eb[CURL_ERROR_SIZE]; } worker_t;
static slot_t slots[NSLOT];
static worker_t workers[NWORK];
static mutex_t mtx;
static volatile int paused = 0, gen = 0;
static u32 tick = 0, seqn = 0;
static char cache_dir[112];
static int cache_state = 0;   /* 0 sin probar, 1 ok, -1 sin dispositivo */
static mutex_t dmtx;

#ifdef IMGBENCH
static volatile int bench_on;
static void bench_start(void);
static const kimg_t *get_impl(const char *path, int emote);
#endif
#if defined(IMGTRACE) || defined(IMGBENCH)
static u64 t0;
static FILE *trace;
static mutex_t tmtx;
static void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void tlog(const char *fmt, ...) {
    if (!trace) return;
    va_list ap; LWP_MutexLock(tmtx);
    fprintf(trace, "[%7u] ", (unsigned)ticks_to_millisecs(gettime() - t0));
    va_start(ap, fmt); vfprintf(trace, fmt, ap); va_end(ap);
    fputc('\n', trace); fflush(trace); LWP_MutexUnlock(tmtx);
}
#define TLOG(...) tlog(__VA_ARGS__)
#else
#define TLOG(...) do { } while (0)
#endif

/* ---------------------------------------------------------- URLs */
static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
/* Live preview thumbnails change every few minutes: RAM only. */
static int disk_ok(const char *url) {
    if (!starts(url, "https://")) return 0;                 /* companion LAN: ya es KWI1 local */
    if (strstr(url, "/video_thumbnails/") || strstr(url, "/previews-ttv/") || strstr(url, "/livestream")) return 0;
    if (strstr(url, "/cf_vods/") || strstr(url, "vod-secure") || strstr(url, "/thumb/")) return 0;
    return 1;
}
/* Display-size variant published by the CDN. *alt receives the original (fallback). */
static void rewrite(const char *in, char *out, int sz, char *alt, int asz) {
    const char *p;
    snprintf(out, sz, "%s", in); alt[0] = 0;
    if (asz < 1) return;
    /* Twitch: ...-channel_offline_image-1920x1080.png -> -320x180.png (verificado: existe) */
    if (starts(in, "https://static-cdn.jtvnw.net/") && (p = strstr(in, "-channel_offline_image-1920x1080."))) {
        snprintf(out, sz, "%.*s-channel_offline_image-320x180.%s", (int)(p - in), in, p + strlen("-channel_offline_image-1920x1080."));
        snprintf(alt, asz, "%s", in);
        return;
    }
    /* Twitch: avatares 300x300 / 600x600 -> 70x70 */
    if (starts(in, "https://static-cdn.jtvnw.net/") && strstr(in, "-profile_image-") &&
        ((p = strstr(in, "-300x300.")) || (p = strstr(in, "-600x600.")) || (p = strstr(in, "-150x150.")))) {
        snprintf(out, sz, "%.*s-70x70.%s", (int)(p - in), in, p + 9);
        snprintf(alt, asz, "%s", in);
        return;
    }
    /* Kick: avatar fullsize (350x350) -> medium (150x150) */
    if (starts(in, "https://files.kick.com/images/user/") && (p = strstr(in, "-fullsize.webp"))) {
        snprintf(out, sz, "%.*s-medium.webp%s", (int)(p - in), in, p + strlen("-fullsize.webp"));
        snprintf(alt, asz, "%s", in);
        return;
    }
    /* Kick emotes: /static (56x56 WebP, ~1 KB) y si no existe /fullsize (GIF/PNG) */
    if (starts(in, "https://files.kick.com/emotes/") && (p = strstr(in, "/static")) && !p[7]) {
        snprintf(alt, asz, "%.*s/fullsize", (int)(p - in), in);
        return;
    }
}

/* ---------------------------------------------------------- disco */
static unsigned fnv(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static int cache_ready(void) {
    if (cache_state) return cache_state > 0;
    LWP_MutexLock(dmtx);
    if (!cache_state) {
        char d[2][96]; struct stat st;
        if (fs_data_dirs(TV_APP_NAME, d, 1) < 1) cache_state = -1;
        else {
            snprintf(cache_dir, sizeof(cache_dir), "%scache", d[0]);
            mkdir(cache_dir, 0777);   /* pasa por fsguard: un dispositivo ausente da ENODEV */
            cache_state = (!stat(cache_dir, &st) && S_ISDIR(st.st_mode)) ? 1 : -1;
        }
    }
    LWP_MutexUnlock(dmtx);
    return cache_state > 0;
}
static void cache_name(const char *url, char *out, int sz) { snprintf(out, sz, "%s/%08x.img", cache_dir, fnv(url)); }
/* "KWC1" epoch(4) urllen(2) paylen(4) url payload */
static int disk_load(const char *url, worker_t *w) {
    char fn[140]; unsigned char h[14]; int ok = 0;
    if (!cache_ready()) return 0;
    cache_name(url, fn, sizeof(fn));
    LWP_MutexLock(dmtx);
    FILE *f = fopen(fn, "rb");
    if (f) {
        if (fread(h, 1, 14, f) == 14 && !memcmp(h, "KWC1", 4)) {
            long ep = ((long)h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7];
            int ul = (h[8] << 8) | h[9];
            int pl = (h[10] << 24) | (h[11] << 16) | (h[12] << 8) | h[13];
            long long now = sh_clock_now();
            char u[768];
            if (ul > 0 && ul < (int)sizeof(u) && pl > 8 && pl <= w->cap && now - ep < DISK_TTL && now - ep > -86400 &&
                fread(u, 1, ul, f) == (size_t)ul) {
                u[ul] = 0;
                if (!strcmp(u, url) && fread(w->buf, 1, pl, f) == (size_t)pl) { w->n = pl; ok = 1; }
            }
        }
        fclose(f);
        if (!ok) unlink(fn);
    }
    LWP_MutexUnlock(dmtx);
    return ok;
}
static void disk_save(const char *url, const unsigned char *data, int n, const kimg_t *im) {
    char fn[140]; unsigned char h[14];
    if (!cache_ready()) return;
    int ul = strlen(url), raw = 8 + im->w * im->h * 3, use_raw = raw < n;
    int pl = use_raw ? raw : n;
    long ep = (long)sh_clock_now();
    memcpy(h, "KWC1", 4);
    h[4] = ep >> 24; h[5] = ep >> 16; h[6] = ep >> 8; h[7] = ep;
    h[8] = ul >> 8; h[9] = ul; h[10] = pl >> 24; h[11] = pl >> 16; h[12] = pl >> 8; h[13] = pl;
    cache_name(url, fn, sizeof(fn));
    LWP_MutexLock(dmtx);
    FILE *f = fopen(fn, "wb");
    if (f) {
        int good = fwrite(h, 1, 14, f) == 14 && fwrite(url, 1, ul, f) == (size_t)ul;
        if (good && !use_raw) good = fwrite(data, 1, n, f) == (size_t)n;
        else if (good) {
            unsigned char hd[8] = { 'K', 'W', 'I', '1', im->w >> 8, im->w, im->h >> 8, im->h };
            unsigned char row[320 * 3];
            good = fwrite(hd, 1, 8, f) == 8;
            for (int y = 0; good && y < im->h; y++) {
                const u32 *s = im->px + y * im->w;
                for (int x = 0; x < im->w; x++) { row[x * 3] = s[x] >> 16; row[x * 3 + 1] = s[x] >> 8; row[x * 3 + 2] = s[x]; }
                good = fwrite(row, 1, im->w * 3, f) == (size_t)(im->w * 3);
            }
        }
        if (fclose(f) || !good) unlink(fn);
    }
    LWP_MutexUnlock(dmtx);
}
/* Una vez por sesion, con los hilos ociosos: borra lo vencido; si hay demasiados archivos, todo. */
static void disk_sweep(void) {
    static int done = 0;
    if (done || !cache_ready()) return;
    done = 1;
    LWP_MutexLock(dmtx);
    DIR *d = opendir(cache_dir); struct dirent *e; int n = 0, del = 0;
    char fn[200]; long long now = sh_clock_now();
    if (d) {
        while ((e = readdir(d)) != NULL && n < 3000) {
            if (e->d_name[0] == '.') continue;
            n++;
            snprintf(fn, sizeof(fn), "%s/%s", cache_dir, e->d_name);
            FILE *f = fopen(fn, "rb"); unsigned char h[8]; int bad = 1;
            if (f) { if (fread(h, 1, 8, f) == 8 && !memcmp(h, "KWC1", 4)) { long ep = ((long)h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7]; bad = now - ep >= DISK_TTL || now - ep < -86400; } fclose(f); }
            if (bad || n > 1200) { unlink(fn); del++; }
        }
        closedir(d);
    }
    LWP_MutexUnlock(dmtx);
    TLOG("cache sweep: %d archivos, %d borrados", n, del); (void)del;
}

/* ---------------------------------------------------------- red */
static size_t wr(char *p, size_t s, size_t n, void *u) {
    worker_t *w = (worker_t *)u;
    size_t k = s * n;
    if (w->n + (int)k > w->lim) return 0;   /* emote GIF enorme: se queda como texto */
    if (w->n + (int)k > w->cap) {
        int nc = w->cap > 0 ? w->cap : 256 * 1024; while (nc < w->n + (int)k) nc *= 2;
        if (nc > MAXIMG || w->n + (int)k > w->lim) return 0;
        unsigned char *nb = realloc(w->buf, nc);
        if (!nb) return 0;
        w->buf = nb; w->cap = nc;
    }
    memcpy(w->buf + w->n, p, k);
    w->n += k;
    return k;
}
typedef struct { int epoch, emote; } prog_t;
static int image_progress(void *v, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    prog_t *p = (prog_t *)v;
    return (paused && !p->emote) || p->epoch != gen;
}
static int net_get(worker_t *w, const char *url, int epoch, int emote, long *code_out) {
    char eb[CURL_ERROR_SIZE]; long code = 0; prog_t pr = { epoch, emote };
    if (!w->c) {
        w->c = curl_easy_init();
        if (!w->c) return 0;
    }
    CURL *c = w->c;
    sh_curl_common(c, eb);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, image_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, w);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);          /* banners grandes por Wi-Fi */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 6L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_MAXCONNECTS, 4L);
    w->lim = emote ? EMOTE_MAX : MAXIMG;
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE, (long)w->lim);
    w->n = 0;
    int res = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    *code_out = code;
#if defined(IMGTRACE) || defined(IMGBENCH)
    double tc = 0, ta = 0, tt = 0; long nc = 0;
    curl_easy_getinfo(c, CURLINFO_CONNECT_TIME, &tc); curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &ta);
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &tt); curl_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc);
    TLOG("w%d net http=%ld curl=%d bytes=%d newconn=%ld tcp=%.0fms tls=%.0fms total=%.0fms %s", w->id, code, res, w->n, nc, tc * 1000, ta * 1000, tt * 1000, url);
#endif
    if (res != CURLE_OK) { curl_easy_cleanup(w->c); w->c = NULL; }   /* conexion rota: empezar limpio */
    return res == CURLE_OK && code == 200 && w->n >= 8;
}
static int decode_buf(const unsigned char *buf, int n, kimg_t *out) {
    if (n < 8) return 0;
    if (memcmp(buf, "KWI1", 4)) return image_decode(buf, n, out);
    int w = (buf[4] << 8) | buf[5], h = (buf[6] << 8) | buf[7];
    if (w <= 0 || h <= 0 || w > 640 || h > 360 || n < 8 + w * h * 3) return 0;
    u32 *px = malloc(w * h * 4);
    if (!px) return 0;
    const unsigned char *s = buf + 8;
    for (int i = 0; i < w * h; i++, s += 3) px[i] = (s[0] << 16) | (s[1] << 8) | s[2];
    out->w = w; out->h = h; out->px = px;
    return 1;
}
/* Emotes: a lo sumo EMOTE_H px de alto (se dibujan a la altura de la linea). */
static void shrink_emote(kimg_t *im) {
    if (im->h <= EMOTE_H || !im->px) return;
    int nw = im->w * EMOTE_H / im->h; if (nw < 1) nw = 1;
    u32 *px = malloc(nw * EMOTE_H * 4);
    if (!px) return;
    for (int y = 0; y < EMOTE_H; y++) for (int x = 0; x < nw; x++) px[y * nw + x] = im->px[(y * im->h / EMOTE_H) * im->w + x * im->w / nw];
    free(im->px); im->px = px; im->w = nw; im->h = EMOTE_H;
}
static int fetch_img(worker_t *w, const char *url, kimg_t *out, int epoch, int emote) {
    char u1[900], u2[900]; long code = 0;
    u64 s0 = gettime(); (void)s0;
    int dok = disk_ok(url);
    if (dok && disk_load(url, w)) {
        int ok = decode_buf(w->buf, w->n, out);
        TLOG("w%d disk ok=%d bytes=%d %dx%d %ums %s", w->id, ok, w->n, out->w, out->h, (unsigned)ticks_to_millisecs(gettime() - s0), url);
        if (ok) { if (emote) shrink_emote(out); return 1; }
    }
    rewrite(url, u1, sizeof(u1), u2, sizeof(u2));
    int got = net_get(w, u1, epoch, emote, &code);
    if (!got && u2[0] && epoch == gen && (!paused || emote)) got = net_get(w, u2, epoch, emote, &code);
    if (!got) return 0;
    u64 d0 = gettime(); (void)d0;
    int ok = decode_buf(w->buf, w->n, out);
    TLOG("w%d decode ok=%d %dx%d %ums (total %ums) %s", w->id, ok, out->w, out->h, (unsigned)ticks_to_millisecs(gettime() - d0), (unsigned)ticks_to_millisecs(gettime() - s0), url);
    if (ok && emote) shrink_emote(out);
    if (ok && dok) disk_save(url, w->buf, w->n, out);
    if (w->cap > 512 * 1024) { unsigned char *nb = realloc(w->buf, 256 * 1024); if (nb) { w->buf = nb; w->cap = 256 * 1024; } }
    return ok;
}

static int pending_locked(void) {
    int n = 0;
    for (int i = 0; i < NSLOT; i++) if (slots[i].state == S_WANT || slots[i].state == S_LOADING) n++;
    return n;
}
static void *thread(void *a) {
    worker_t *w = (worker_t *)a;
    int idle_logged = 1; (void)idle_logged;
    while (1) {
        int i, pick = -1, g;
        char url[940];
        LWP_MutexLock(mtx);
        for (i = 0; i < NSLOT; i++) {
            slot_t *s = &slots[i];
            if (s->state != S_WANT) continue;
            if (paused && !(s->emote && w->id == 0)) continue;       /* durante el video solo emotes, de a uno */
            if (s->emote && w->id != 0 && !paused) continue;        /* emotes: siempre en un solo hilo */
            if (pick < 0 || s->used > slots[pick].used || (s->used == slots[pick].used && s->seq < slots[pick].seq)) pick = i;
        }
        if (pick >= 0) {
            w->active = !slots[pick].emote ? 1 : 2;
            slots[pick].state = S_LOADING;
            if (!strncmp(slots[pick].path, "https://", 8)) snprintf(url, sizeof(url), "%s", slots[pick].path);
            else snprintf(url, sizeof(url), "%s%s", k_server, slots[pick].path);
        }
        int emote = pick >= 0 ? slots[pick].emote : 0;
        g = gen;
#if defined(IMGTRACE) || defined(IMGBENCH)
        int pend = pending_locked();
#endif
        LWP_MutexUnlock(mtx);
        if (pick < 0) {
#if defined(IMGTRACE) || defined(IMGBENCH)
            if (!idle_logged && !pend && w->id == 0) { TLOG("IDLE: nada pendiente"); idle_logged = 1; }
#endif
            if (w->id == 0 && !paused) disk_sweep();
            usleep(30000); continue;
        }
        idle_logged = 0;
        kimg_t im = { 0, 0, NULL };
        int ok = fetch_img(w, url, &im, g, emote);
        LWP_MutexLock(mtx);
        if (g != gen || slots[pick].state != S_LOADING) { free(im.px); }
        else if (!ok && paused && !emote) { free(im.px); slots[pick].state = S_WANT; }
        else if (ok) { slots[pick].img = im; slots[pick].state = S_READY; }
        else { free(im.px); slots[pick].state = S_FAIL; slots[pick].failures++; slots[pick].retry = gettime() + millisecs_to_ticks(emote ? 600000 : slots[pick].failures < 3 ? 5000 : 60000); }
        w->active = 0;
        LWP_MutexUnlock(mtx);
        usleep(emote ? 20000 : 2000);   /* emotes: dejar respirar al video/chat */
    }
    return NULL;
}

void thumbs_flush(void);
#ifdef IMGBENCH
#include "imgbench.inc"
#endif

void thumbs_init(void) {
    memset(slots, 0, sizeof(slots));
    LWP_MutexInit(&mtx, false);
    LWP_MutexInit(&dmtx, false);
#if defined(IMGTRACE) || defined(IMGBENCH)
    LWP_MutexInit(&tmtx, false);
    t0 = gettime();
    { char d[2][96], fn[128]; if (fs_data_dirs(TV_APP_NAME, d, 1) > 0) { snprintf(fn, sizeof(fn), "%simgtrace.log", d[0]); trace = fopen(fn, "w"); } }
    TLOG("thumbs rc13: %d hilos, %d slots, cache disco", NWORK, NSLOT);
#endif
    for (int i = 0; i < NWORK; i++) {
        lwp_t th;
        workers[i].id = i; workers[i].cap = 256 * 1024; workers[i].buf = malloc(workers[i].cap);
        if (!workers[i].buf) workers[i].cap = 0;
        LWP_CreateThread(&th, thread, &workers[i], NULL, 128 * 1024, 30);
    }
#ifdef IMGBENCH
    bench_start();
#endif
}
static int any_active(int normal_only) {
    for (int i = 0; i < NWORK; i++) if (workers[i].active == 1 || (!normal_only && workers[i].active)) return 1;
    return 0;
}
void thumbs_pause(int p) {
    LWP_MutexLock(mtx); paused = p; LWP_MutexUnlock(mtx);
    /* Do not enter another curl transfer until the workers released their image handles
       (emotes are tiny and keep going while the video plays). */
    while (p && any_active(1)) { ui_busy_tick(); usleep(10000); }
}
void thumbs_frame(void) { tick++; }   /* una vez por cuadro dibujado */

static const kimg_t *get_impl(const char *path, int emote) {
    int i, fr = -1, old = -1;
    const kimg_t *r = NULL;
    if (!path || !path[0] || (!k_server[0] && strncmp(path, "https://", 8))) return NULL;
    LWP_MutexLock(mtx);
    for (i = 0; i < NSLOT; i++) {
        if (slots[i].state != S_FREE && !strcmp(slots[i].path, path)) {
            slots[i].used = tick;
            if (slots[i].state == S_FAIL && (!paused || emote) && gettime() >= slots[i].retry) slots[i].state = S_WANT;
            r = slots[i].state == S_READY ? &slots[i].img : NULL;
            LWP_MutexUnlock(mtx);
            return r;
        }
        if (slots[i].state == S_FREE && fr < 0) fr = i;
        if (slots[i].state != S_LOADING && (old < 0 || slots[i].used < slots[old].used)) old = i;
    }
    if (fr < 0 && old >= 0 && tick - slots[old].used > 2) {   /* no expulsar algo que se esta mostrando */
        fr = old;
        free(slots[fr].img.px); slots[fr].img.px = NULL;
    }
    if (fr >= 0) {
        snprintf(slots[fr].path, sizeof(slots[fr].path), "%s", path);
        slots[fr].failures = 0; slots[fr].retry = 0;
        slots[fr].state = S_WANT; slots[fr].emote = emote;
        slots[fr].used = tick; slots[fr].seq = seqn++;
    }
    LWP_MutexUnlock(mtx);
    return NULL;
}
#ifdef IMGBENCH
const kimg_t *thumb_get(const char *path) { return bench_on ? NULL : get_impl(path, 0); }
#else
const kimg_t *thumb_get(const char *path) { return get_impl(path, 0); }
#endif
const kimg_t *emote_get(const char *url) { return get_impl(url, 1); }

void thumbs_flush(void) {
    int i;
    LWP_MutexLock(mtx);
    gen++;
    for (i = 0; i < NSLOT; i++) {
        if (slots[i].state == S_LOADING) { slots[i].state = S_FREE; continue; }
        free(slots[i].img.px); slots[i].img.px = NULL;
        slots[i].state = S_FREE;
    }
    LWP_MutexUnlock(mtx);
}

int thumbs_pending(void) {
    LWP_MutexLock(mtx);
    int n = pending_locked();
    LWP_MutexUnlock(mtx);
    return n;
}
