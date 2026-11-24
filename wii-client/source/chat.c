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
#include "guard.h"
#include "dlog.h"

#define NMSG 24
static tchat_t ring[NMSG];
static int head = 0, count = 0;
static volatile int gen = 0, run = 0;
static char login[48];
static char authpass[2048],authnick[64],outgoing[351],outgoing_tag[600];
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

/* rc11: el hilo principal nunca espera sin limite al hilo del chat (ver guard.c). A los 1,5 s se le
 * cortan los sockets; a los 4 s se lo abandona (zombi) y el chat nuevo arranca cuando termine. */
static volatile int th_done = 1; static lwp_t th_zombie = LWP_THREAD_NULL;
static int pending_start; static char pending_who[48];
static void *(*th_body)(void *);
static void *th_wrap(void *a) { th_body(a); th_done = 1; return NULL; }
static int reap(void) { if (th_zombie == LWP_THREAD_NULL) return 1; if (!th_done) return 0; LWP_JoinThread(th_zombie, NULL); th_zombie = LWP_THREAD_NULL; dlog("chat: el hilo abandonado termino"); return 1; }
void chat_stop(void) {
    pending_start = 0;
    if (th != LWP_THREAD_NULL) {
        run = 0; u64 t0 = gettime(); int killed = 0; guard_where("chat_stop");
        while (!th_done) {
            u32 ms = ticks_to_millisecs(gettime() - t0);
            if (ms > 1500 && !killed) { killed = 1; int n = guard_kill_socks(th); dlog("chat: el hilo no termina en 1,5 s; corto %d socket(s)", n); }
            if (ms > 4000) { dlog("chat: hilo trabado; no se lo espera (zombi)"); th_zombie = th; break; }
            guard_beat(); usleep(10000);
        }
        if (th_zombie != th) LWP_JoinThread(th, NULL);
        th = LWP_THREAD_NULL;
    }
    write_ready=0;outgoing[0]=0;rejected=0;memset(authpass,0,sizeof(authpass));authenticated=0;
}

void chat_start(const char *who) {
    chat_stop();
    if (!mtx_ok) { LWP_MutexInit(&mtx, false); mtx_ok = 1; }
    if (!reap()) { snprintf(pending_who, sizeof(pending_who), "%s", who); pending_start = 1; snprintf(status, sizeof(status), "Chat: cerrando la conexion anterior..."); return; }
    LWP_MutexLock(mtx); head = count = 0; gen++; LWP_MutexUnlock(mtx);
    snprintf(login, sizeof(login), "%s", who);
    authenticated=auth_irc_snapshot(authpass,sizeof(authpass),authnick,sizeof(authnick));
    snprintf(status, sizeof(status), "conectando...");
    run = 1;
    th_body = k_server[0] && !authenticated ? chat_thread : direct_thread; th_done = 0;
    if (LWP_CreateThread(&th, th_wrap, NULL, NULL, 160 * 1024, 44) < 0) { th = LWP_THREAD_NULL; run = 0; th_done = 1; }
}

int chat_gen(void) { return gen; }
const char *chat_status(void) { return status; }

int chat_get(tchat_t *out, int max) {
    int i, n;
    if (pending_start && th_done && th == LWP_THREAD_NULL) { char w[48]; snprintf(w, sizeof w, "%s", pending_who); pending_start = 0; chat_start(w); }
    if (!mtx_ok) return 0;
    LWP_MutexLock(mtx);
    n = count < max ? count : max;
    for (i = 0; i < n; i++) out[i] = ring[(head + count - n + i) % NMSG];
    LWP_MutexUnlock(mtx);
    return n;
}

/* The UI only enqueues; the IRC worker is the sole socket owner/writer. */
/* rc11: tag = rangos de emotes calculados en la Wii (formato del tag IRC "emotes").
 * Twitch no devuelve por IRC el mensaje propio, asi que el eco local se dibuja con esto. */
int chat_send_text_e(const char *text, const char *tag) {
 if(!mtx_ok||!auth_message_valid(text))return 0;
#ifdef SEND_DRYRUN
 /* rc8 PRUEBA: no se envia nada; se guarda la linea IRC que se mandaria */
 {FILE*f=fopen("sd:/twitchwii-test/dryrun-payload.txt","a");if(f){fprintf(f,"PRIVMSG #%s :%s\n",login,text);fclose(f);}push_e(authnick[0]?authnick:"yo",text,C_ACCENT,0,tag);return 1;}
#endif
 LWP_MutexLock(mtx);int ok=run&&authenticated&&write_ready&&!outgoing[0]&&(!last_send||ticks_to_millisecs(gettime()-last_send)>=2000);
 if(ok){snprintf(outgoing,sizeof(outgoing),"%s",text);snprintf(outgoing_tag,sizeof(outgoing_tag),"%s",tag?tag:"");last_send=gettime();}LWP_MutexUnlock(mtx);return ok;
}
int chat_send_text(const char *text) { return chat_send_text_e(text, NULL); }
void chat_local_echo_e(const char *name,const char *text,const char *tag){if(mtx_ok)push_e(name,text,C_ACCENT,0,tag);}
void chat_local_echo(const char *name,const char *text){if(mtx_ok)push(name,text,C_ACCENT,0);}

int chat_auth_rejected(void){if(!mtx_ok)return 0;LWP_MutexLock(mtx);int r=rejected;LWP_MutexUnlock(mtx);return r;}

#ifdef AUTOTEST_SOAK
static const char *soak_id[]={"emotesv2_004b58fc8f3b41f7b48a4c06aeff1540","emotesv2_0b84aba793f74b429b5f4c0375f1c8cb","emotesv2_155841205da744199051303d7421a19e","emotesv2_2ac70f7402324cd0926246c9d8a37ba2","emotesv2_3a5b59f49ec2433bb76a0071ec4997bb","emotesv2_408f173c2508459c9055bc9d6addd166","emotesv2_49ae8b88111a4b34826575368091fc56","emotesv2_507e418dc024418eacfb92751d345ea7","emotesv2_584a7f8bd36046b1ad96eb2d7ab8a5e8","emotesv2_64820eca5e6444479e04e77c1356bca4","emotesv2_695dc2c4d953441ab37a510befd00194","emotesv2_6a06e06a83a3442cb3784528d5d7c674","emotesv2_7573eea6f0e74d91a42d22077b017a03","emotesv2_9de08b0452e24e3a9c4e75c7b5adf263","emotesv2_c13937fb5d0341ee9fb0b7da1a6ad13b","emotesv2_c2f18c53b60f4ab6a3141d52f5c741fd","emotesv2_d210039682c6418086b5194ca24e70bd","emotesv2_d9dc5705d728493ebf3f0ae0be3fc578","emotesv2_e2fe99e21a5a4e8891c5d16bc893a4be","emotesv2_e3d80f41d5d8441d94f83b549ef68706","emotesv2_f95ba7ced8bc4c19886ceafc3294c1f7"};
static const char *soak_tok[]={"ohneDrilla","ohneRIP","ohneLamonting","kaicSturdyy","xqcSlam","kaicNaenae","ohnePIXEL","kaicWAVE","ohneCool","ohneWAS","ohneExplode","kaicFireMGM","kaicGimmiegold","ohneGABE","xqcDitch","xqcCursed","xqcSus","xqcCheer","ohneJeepers","ohneVas","ohnePls"};
/* rc8 soak: 3 emotes (animados casi todos) por mensaje, rotando por la lista */
void chat_test_soak(int k){char t[400],tag[600];int n=sizeof(soak_id)/sizeof(soak_id[0]),p=0,q=0;if(!mtx_ok)return;
 p=snprintf(t,sizeof t,"soak%d ",k%10);
 for(int j=0;j<3;j++){int e=(k*3+j)%n,L=strlen(soak_tok[e]);q+=snprintf(tag+q,sizeof tag-q,"%s%s:%d-%d",j?"/":"",soak_id[e],p,p+L-1);p+=snprintf(t+p,sizeof t-p,"%s ",soak_tok[e]);}
 snprintf(t+p,sizeof t-p,"Kappa");q+=snprintf(tag+q,sizeof tag-q,"/25:%d-%d",p,p+4);
 push_e("Soak",t,0x53FC18,0,tag);}
#endif
#ifdef AUTOTEST_EMOTES
void chat_test_inject(void){if(!mtx_ok)return;
 /* "Kappa hola LUL animado PogChamp y un texto largo para probar el corte de linea Kappa" */
 push_e("PruebaEmotes","Kappa hola LUL animado catJAM y un texto largo para probar el corte de linea Kappa",0x53FC18,0,"25:0-4,77-81/425618:11-13/emotesv2_dcd06b30a5c24f6eb871e8f5edbd44f7:23-28");
 push_e("Otro","\xc3\xa1\xc3\xa9 Kappa Kappa",0xFF7A00,0,"25:3-7,9-13");}
#endif
