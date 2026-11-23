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

/* rc7: tag IRC "emotes=25:0-4,12-16/1902:6-10" (indices en caracteres) -> rangos en bytes de text. */
static void parse_emotes(tchat_t *m, const char *tag) {
    m->ne = 0;
    if (!tag || !tag[0] || m->sys) return;
    int tl = strlen(m->text);
    /* indice de caracter -> byte */
    static short cpos[201]; int nc = 0, b = 0;
    while (b < tl && nc < 200) { cpos[nc++] = b; unsigned char c = m->text[b]; b += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4; }
    cpos[nc] = tl;
    const char *p = tag;
    while (*p && m->ne < 8) {
        char id[48]; int k = 0;
        while (*p && *p != ':' && k < 47) id[k++] = *p++;
        id[k] = 0;
        if (*p != ':') break;
        p++;
        while (*p && *p != '/') {
            int s0 = (int)strtol(p, (char **)&p, 10), e0 = -1;
            if (*p == '-') { p++; e0 = (int)strtol(p, (char **)&p, 10); }
            if (e0 >= s0 && s0 >= 0 && e0 < nc && m->ne < 8) {
                int bs = cpos[s0], be = cpos[e0 + 1], ok = be > bs && be - bs < 64;
                for (int q = bs; ok && q < be; q++) if (m->text[q] == ' ') ok = 0;
                for (const char *q = id; ok && *q; q++) if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '_')) ok = 0;
                if (ok) {   /* ordenados por posicion */
                    int at = m->ne;
                    while (at > 0 && m->eat[at - 1] > bs) { m->eat[at] = m->eat[at - 1]; m->elen[at] = m->elen[at - 1]; memcpy(m->eid[at], m->eid[at - 1], 48); at--; }
                    m->eat[at] = bs; m->elen[at] = be - bs; snprintf(m->eid[at], 48, "%s", id); m->ne++;
                }
            }
            if (*p == ',') p++;
            else if (*p != '/') { while (*p && *p != '/') p++; }
        }
        if (*p == '/') p++;
    }
}
int emote_url(const char *id, char *out, int cap) {
    if (!id || !id[0] || strlen(id) > 47) return 0;
    snprintf(out, cap, "https://static-cdn.jtvnw.net/emoticons/v2/%s/static/dark/1.0", id);   /* static = primer cuadro, 28 px */
    return 1;
}
static void push_e(const char *name, const char *text, u32 color, int sys, const char *emotes) {
    LWP_MutexLock(mtx);
    tchat_t *m = &ring[(head + count) % NMSG];
    if (count == NMSG) { head = (head + 1) % NMSG; m = &ring[(head + count - 1) % NMSG]; }
    else count++;
    snprintf(m->name, sizeof(m->name), "%s", name);
    snprintf(m->text, sizeof(m->text), "%s", text);
    m->sys = sys; parse_emotes(m, emotes);
    m->color = color ? color : 0x9146FF;
    m->sys = sys;
    gen++;
    LWP_MutexUnlock(mtx);
}
static void push(const char *name, const char *text, u32 color, int sys) { push_e(name, text, color, sys, NULL); }

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

#ifdef AUTOTEST_EMOTES
void chat_test_inject(void){if(!mtx_ok)return;
 /* "Kappa hola LUL animado PogChamp y un texto largo para probar el corte de linea Kappa" */
 push_e("PruebaEmotes","Kappa hola LUL animado catJAM y un texto largo para probar el corte de linea Kappa",0x53FC18,0,"25:0-4,77-81/425618:11-13/emotesv2_dcd06b30a5c24f6eb871e8f5edbd44f7:23-28");
 push_e("Otro","\xc3\xa1\xc3\xa9 Kappa Kappa",0xFF7A00,0,"25:3-7,9-13");}
#endif
