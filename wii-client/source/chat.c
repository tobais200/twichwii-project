/*
 * chat.c - TwitchWii: chat de SOLO LECTURA mientras se mira un directo.
 * El servidor companion se conecta al IRC de Twitch como usuario anonimo ("justinfan")
 * y la Wii le pide los mensajes nuevos cada ~1,5 s (GET /wii/chat/<canal>?since=N)
 * desde un hilo de baja prioridad. No se puede escribir (no hay login).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>
#include <curl/curl.h>
#include "tv.h"

#define NMSG 24
static tchat_t ring[NMSG];
static int head = 0, count = 0;
static volatile int gen = 0, run = 0;
static char login[48];
static char authpass[2048],authnick[64],outgoing[351];
static volatile int authenticated,write_ready;static int rejected;static u64 last_send;
static char status[96] = "";
static mutex_t mtx;
static int mtx_ok = 0;
static lwp_t th = LWP_THREAD_NULL;

static u32 parse_color(const char *c) {
    if (c[0] != '#' || strlen(c) < 7) return 0;
    u32 v = (u32)strtoul(c + 1, NULL, 16) & 0xFFFFFF;
    /* colores muy oscuros: aclarar para que se lean sobre el fondo negro (como hace Twitch) */
    int r = v >> 16, g = (v >> 8) & 255, b = v & 255;
    if (r * 3 + g * 6 + b < 600) v = gfx_mix(v, 0xFFFFFF, 110);
    return v;
}

static void push(const char *name, const char *text, u32 color, int sys) {
    LWP_MutexLock(mtx);
    tchat_t *m = &ring[(head + count) % NMSG];
    if (count == NMSG) { head = (head + 1) % NMSG; m = &ring[(head + count - 1) % NMSG]; }
    else count++;
    snprintf(m->name, sizeof(m->name), "%s", name);
    snprintf(m->text, sizeof(m->text), "%s", text);
    m->color = color ? color : 0x9146FF;
    m->sys = sys;
    gen++;
    LWP_MutexUnlock(mtx);
}

#include "irc_direct.inc"

static void *chat_thread(void *a) {
    (void)a;
    long long since = 0;
    char url[256];
    while (run) {
        sh_resp_t r;
        snprintf(url, sizeof(url), "%s/wii/chat/%s?since=%lld&limit=12", k_server, login, since);
        sh_request("GET", url, NULL, NULL, NULL, &r);
        if (!run) { sh_resp_free(&r); break; }
        if (r.res == 0 && r.code == 200 && r.data) {
            const char *d = jws(r.data), *arr = jget(d, "messages"), *e;
            char st[64] = "";
            const char *sv = jget(d, "status");
            if (sv) jstr(sv, st, sizeof(st));
            snprintf(status, sizeof(status), "%s", st);
            for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
                char nm[28] = "", tx[200] = "", co[12] = "";
                const char *v;
                if ((v = jget(e, "id"))) { long long id = jnum(v); if (id > since) since = id; }
                if ((v = jget(e, "name")) && !jis_null(v)) jstr(v, nm, sizeof(nm));
                if ((v = jget(e, "text")) && !jis_null(v)) jstr(v, tx, sizeof(tx));
                if ((v = jget(e, "color")) && !jis_null(v)) jstr(v, co, sizeof(co));
                v = jget(e, "sys");
                push(nm, tx, parse_color(co), v && *jws(v) == 't');
            }
        } else snprintf(status, sizeof(status), "sin respuesta del servidor");
        sh_resp_free(&r);
        int i;
        for (i = 0; i < 15 && run; i++) usleep(100 * 1000);
    }
    return NULL;
}

void chat_stop(void) {
    if (th != LWP_THREAD_NULL) { run = 0; LWP_JoinThread(th, NULL); th = LWP_THREAD_NULL; }
    write_ready=0;outgoing[0]=0;rejected=0;memset(authpass,0,sizeof(authpass));authenticated=0;
}

void chat_start(const char *who) {
    chat_stop();
    if (!mtx_ok) { LWP_MutexInit(&mtx, false); mtx_ok = 1; }
    LWP_MutexLock(mtx); head = count = 0; gen++; LWP_MutexUnlock(mtx);
    snprintf(login, sizeof(login), "%s", who);
    authenticated=auth_irc_snapshot(authpass,sizeof(authpass),authnick,sizeof(authnick));
    snprintf(status, sizeof(status), "conectando...");
    run = 1;
    if (LWP_CreateThread(&th, k_server[0] && !authenticated ? chat_thread : direct_thread, NULL, NULL, 160 * 1024, 44) < 0) { th = LWP_THREAD_NULL; run = 0; }
}

int chat_gen(void) { return gen; }
const char *chat_status(void) { return status; }

int chat_get(tchat_t *out, int max) {
    int i, n;
    if (!mtx_ok) return 0;
    LWP_MutexLock(mtx);
    n = count < max ? count : max;
    for (i = 0; i < n; i++) out[i] = ring[(head + count - n + i) % NMSG];
    LWP_MutexUnlock(mtx);
    return n;
}

/* The UI only enqueues; the IRC worker is the sole socket owner/writer. */
int chat_send_text(const char *text) {
 if(!mtx_ok||!auth_message_valid(text))return 0;
 LWP_MutexLock(mtx);int ok=run&&authenticated&&write_ready&&!outgoing[0]&&(!last_send||ticks_to_millisecs(gettime()-last_send)>=2000);
 if(ok){snprintf(outgoing,sizeof(outgoing),"%s",text);last_send=gettime();}LWP_MutexUnlock(mtx);return ok;
}
void chat_local_echo(const char *name,const char *text){if(mtx_ok)push(name,text,C_ACCENT,0);}

int chat_auth_rejected(void){if(!mtx_ok)return 0;LWP_MutexLock(mtx);int r=rejected;LWP_MutexUnlock(mtx);return r;}
