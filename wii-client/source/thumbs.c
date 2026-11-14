/* Async, bounded LRU: companion KWI1 or direct HTTPS JPEG/PNG/WebP. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include <curl/curl.h>
#include "tv.h"
#include "image_decode.h"

#define NSLOT 24
#define MAXIMG (1536 * 1024)
enum { S_FREE = 0, S_WANT, S_LOADING, S_READY, S_FAIL };
typedef struct {
    char path[768];
    int state;
    u64 retry;unsigned failures;
    u32 used;       /* ultimo "tick" en que se pidio */
    kimg_t img;
} slot_t;
static slot_t slots[NSLOT];
static mutex_t mtx;
static lwp_t th = LWP_THREAD_NULL;
static volatile int paused = 0, gen = 0, active = 0;
static u32 tick = 0;
#ifdef AUTOTEST
static long trace_http;static int trace_curl,trace_bytes;
#endif

typedef struct { unsigned char *d; int n; } buf_t;
static size_t wr(char *p, size_t s, size_t n, void *u) {
    buf_t *b = (buf_t *)u;
    size_t k = s * n;
    if (b->n + (int)k > MAXIMG) return 0;
    memcpy(b->d + b->n, p, k);
    b->n += k;
    return k;
}

static int image_progress(void *epoch, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return paused || *(const int *)epoch != gen;
}
static int fetch_img(const char *url, kimg_t *out, int epoch) {
    static unsigned char *buf = NULL;
    buf_t b;
    char eb[CURL_ERROR_SIZE];
    long code = 0;
    if (!buf) buf = malloc(MAXIMG);
    if (!buf) return 0;
    b.d = buf; b.n = 0;
    CURL *c = curl_easy_init();
    if (!c) return 0;
    sh_curl_common(c, eb);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, image_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &epoch);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);
    int res = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
#ifdef AUTOTEST
    trace_http=code;trace_curl=res;trace_bytes=b.n;
#endif
    if (res != CURLE_OK || code != 200 || b.n < 8) return 0;
    if (memcmp(buf, "KWI1", 4)) return image_decode(buf, b.n, out);
    int w = (buf[4] << 8) | buf[5], h = (buf[6] << 8) | buf[7];
    if (w <= 0 || h <= 0 || w > 640 || h > 360 || b.n < 8 + w * h * 3) return 0;
    u32 *px = malloc(w * h * 4);
    if (!px) return 0;
    const unsigned char *s = buf + 8;
    int i;
    for (i = 0; i < w * h; i++, s += 3) px[i] = (s[0] << 16) | (s[1] << 8) | s[2];
    out->w = w; out->h = h; out->px = px;
    return 1;
}

static void *thread(void *a) {
    (void)a;
    while (1) {
        int i, pick = -1, g;
        char url[940];
        if (paused) { usleep(100000); continue; }
        LWP_MutexLock(mtx);
        if (paused) { LWP_MutexUnlock(mtx); continue; }
        for (i = 0; i < NSLOT; i++)
            if (slots[i].state == S_WANT && (pick < 0 || slots[i].used > slots[pick].used)) pick = i;   /* lo mas reciente primero */
        if (pick >= 0) {
            active = 1;
            slots[pick].state = S_LOADING;
            if (!strncmp(slots[pick].path, "https://", 8))
                snprintf(url, sizeof(url), "%s", slots[pick].path);
            else snprintf(url, sizeof(url), "%s%s", k_server, slots[pick].path);
        }
        g = gen;
        LWP_MutexUnlock(mtx);
        if (pick < 0) { usleep(40000); continue; }
        kimg_t im = { 0, 0, NULL };
        int ok = fetch_img(url, &im, g);
#ifdef AUTOTEST
        FILE *fl=fopen("sd:/twitchwii-test/images.log","a");
        if(fl){fprintf(fl,"ok=%d http=%ld curl=%d bytes=%d %dx%d %s\n",ok,trace_http,trace_curl,trace_bytes,im.w,im.h,url);fclose(fl);}
#endif
        LWP_MutexLock(mtx);
        if (g != gen || slots[pick].state != S_LOADING) { free(im.px); }
        else if (paused) { free(im.px); slots[pick].state = S_WANT; }
        else if (ok) { slots[pick].img = im; slots[pick].state = S_READY; }
        else {slots[pick].state = S_FAIL;slots[pick].failures++;slots[pick].retry=gettime()+millisecs_to_ticks(slots[pick].failures<3?5000:60000);}
        active = 0;
        LWP_MutexUnlock(mtx);
        usleep(5000);
    }
    return NULL;
}

void thumbs_init(void) {
    memset(slots, 0, sizeof(slots));
    LWP_MutexInit(&mtx, false);
    LWP_CreateThread(&th, thread, NULL, NULL, 128 * 1024, 30);
}
void thumbs_pause(int p) {
    LWP_MutexLock(mtx); paused = p; LWP_MutexUnlock(mtx);
    /* Do not enter another curl transfer until the worker has released its handle. */
    while (p && active) { ui_busy_tick(); usleep(10000); }
}
void thumbs_frame(void) { tick++; }   /* una vez por cuadro dibujado */

const kimg_t *thumb_get(const char *path) {
    int i, fr = -1, old = -1;
    const kimg_t *r = NULL;
    if (!path || !path[0] || (!k_server[0] && strncmp(path, "https://", 8))) return NULL;
    LWP_MutexLock(mtx);
    for (i = 0; i < NSLOT; i++) {
        if (slots[i].state != S_FREE && !strcmp(slots[i].path, path)) {
            slots[i].used = tick;
            if(slots[i].state==S_FAIL && !paused && gettime()>=slots[i].retry)slots[i].state=S_WANT;
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
        slots[fr].failures=0;slots[fr].retry=0;
        slots[fr].state = S_WANT;
        slots[fr].used = tick;
    }
    LWP_MutexUnlock(mtx);
    return NULL;
}

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
    int i, n = 0;
    LWP_MutexLock(mtx);
    for (i = 0; i < NSLOT; i++) if (slots[i].state == S_WANT || slots[i].state == S_LOADING) n++;
    LWP_MutexUnlock(mtx);
    return n;
}
