/*
 * intro.c - pantalla de arranque de TwitchWii: el splash que eligio el usuario (fondo violeta
 * #9147FF con la palabra "twitch" en blanco y sombra 3D negra, data/intro_logo.kwa) que entra con
 * un rebote, puntos de carga y "Reverie" de Debussy de fondo.
 *
 * Musica: Claude Debussy, "Reverie" (L. 68), arreglo para saxofon soprano y piano, interpretado por
 * David Hernando Vitores (2015), licencia CC BY-SA 4.0, via Wikimedia Commons:
 * https://commons.wikimedia.org/wiki/File:Claude_Debussy_-_R%C3%AAverie_-_Arr_for_Soprano_saxophone_and_piano_-_David_Hernando_Vitores.ogg
 * Recorte de 45 s (desde 0:00), mono 22,05 kHz, IMA-ADPCM 4:1 (data/intro_music.kwad), que se
 * decodifica a PCM en el heap al arrancar.
 *
 * Mientras se ve la intro, la red se inicializa en un hilo aparte.
 * A / 2 / +: saltar la intro (la musica sigue de fondo).  B: saltar y silenciar.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>
#include <wiiuse/wpad.h>
#include <malloc.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "tv.h"
#include "intro_music_kwad.h"
#include "intro_logo_kwa.h"

#define VOICE_INTRO 1
#define MIN_MS 6500          /* duracion minima de la intro si no se salta */
#define MAX_NET_MS 20000     /* despues de esto se sigue aunque la red no responda */
#define BGC 0x9147FF         /* violeta del splash */

static lwp_t net_th = LWP_THREAD_NULL;
static volatile int net_done = 0, net_res = 0;
static int music_on = 0;
static u64 music_t0 = 0;
static int music_ms = 0;
static s16 *pcm = NULL;

static void *net_fn(void *a) { (void)a; net_res = sh_net_ready(); net_done = 1; return NULL; }

void intro_music_stop(void) {
    if (!music_on) return;
    music_on = 0;
    ASND_StopVoice(VOICE_INTRO);
    free(pcm); pcm = NULL;
}

/* la musica se entrega al DSP en bloques de 8 KB (doble buffer de ASND): pasar el
   buffer entero de ~2 MB en una sola voz desborda el DMA (Dolphin: "Oversized range") */
#define MCH 8192
static volatile u32 mpos = 0, mlen = 0;
static void music_cb(s32 v) {
    (void)v;
    if (!music_on || mpos >= mlen) return;
    u32 n = mlen - mpos; if (n > MCH) n = MCH;
    if (ASND_AddVoice(VOICE_INTRO, (void *)((u8 *)pcm + mpos), n) == SND_OK) mpos += n;
}

static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 };
static const int ima_idx[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* KWAD: 'KWAD' + u32 frecuencia + u32 muestras + nibbles IMA-ADPCM (alto primero) */
static int music_decode(int *rate) {
    const u8 *d = intro_music_kwad;
    if (intro_music_kwad_size < 12 || memcmp(d, "KWAD", 4)) return 0;
    *rate = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
    u32 n = (d[8] << 24) | (d[9] << 16) | (d[10] << 8) | d[11], i;
    if (n > (intro_music_kwad_size - 12) * 2) n = (intro_music_kwad_size - 12) * 2;
    n &= ~15u;                                   /* bytes multiplo de 32 */
    pcm = memalign(32, n * 2);
    if (!pcm) return 0;
    int pred = 0, idx = 0;
    for (i = 0; i < n; i++) {
        int c = (i & 1) ? (d[12 + i / 2] & 15) : (d[12 + i / 2] >> 4);
        int st = ima_step[idx], dq = st >> 3;
        if (c & 4) dq += st;
        if (c & 2) dq += st >> 1;
        if (c & 1) dq += st >> 2;
        pred += (c & 8) ? -dq : dq;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        idx += ima_idx[c & 7]; if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        pcm[i] = (s16)pred;
    }
    DCFlushRange(pcm, n * 2);
    return (int)(n * 2);
}

static void music_start(void) {
    int rate = 22050;
    int bytes = music_decode(&rate);
    if (bytes < MCH) return;
    mlen = bytes;
    music_ms = (int)((bytes / 2) * 1000LL / rate);
    ASND_Pause(0);
    mpos = MCH;
    music_on = 1;
    if (ASND_SetVoice(VOICE_INTRO, VOICE_MONO_16BIT, rate, 0, pcm, MCH, 200, 200, music_cb) == SND_OK)
        music_t0 = gettime();
    else music_on = 0;
}

/* llamar de vez en cuando: libera la voz cuando la musica termino */
void intro_music_tick(void) {
    if (music_on && ticks_to_millisecs(gettime() - music_t0) > (u32)music_ms + 500) intro_music_stop();
}

static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float ease_out(float t) { t = clampf(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
static float ease_back(float t) { t = clampf(t); float c = 1.6f; t -= 1; return 1 + (c + 1) * t * t * t + c * t * t; }

static void fade_black(int a) { if (a > 0) gfx_rect_a(0, 0, SCR_W, SCR_H, 0x000000, a > 255 ? 255 : a); }

/* bloque del splash (opaco, mismo violeta que el fondo) escalado con vecino mas cercano, centrado en (cx, cy) */
static void blit_splash(int cx, int cy, float sc) {
    const u8 *p = intro_logo_kwa;
    if (memcmp(p, "KWA1", 4)) return;
    int w = (p[4] << 8) | p[5], h = (p[6] << 8) | p[7];
    const u8 *px = p + 8;
    int dw = (int)(w * sc), dh = (int)(h * sc), x, y;
    if (dw < 2 || dh < 2) return;
    int x0 = cx - dw / 2, y0 = cy - dh / 2;
    for (y = 0; y < dh; y++) {
        int yy = y0 + y; if (yy < 0 || yy >= SCR_H) continue;
        int sy = y * h / dh;
        u32 *row = canvas + yy * SCR_W;
        for (x = 0; x < dw; x++) {
            int xx = x0 + x; if (xx < 0 || xx >= SCR_W) continue;
            const u8 *s = px + (sy * w + x * w / dw) * 4;
            row[xx] = (s[0] << 16) | (s[1] << 8) | s[2];
        }
    }
}

static void draw(int ms, int out_ms) {
    gfx_clear(BGC);
    /* splash: aparece con un pequeno rebote */
    float t = (ms - 200) / 800.0f;
    if (t > 0) {
        float sc = 0.55f + 0.45f * ease_back(t);
        int cy = 205 - (int)(40 * (1 - ease_out(t)));
        blit_splash(SCR_W/2, cy, sc);
    }
    float st = clampf((ms - 1100) / 500.0f);
    if (st > 0) {
        int w = gfx_text_w(&font_bold, "para Wii") + 24;
        gfx_rrect(SCR_W/2 - w / 2, 300, w, 26, 13, gfx_mix(BGC, 0xFFFFFF, (int)(255 * st)), 255);
        gfx_text_c(&font_bold, SCR_W/2, 303, "para Wii", gfx_mix(BGC, 0x000000, (int)(255 * st)));
    }
    if (ms > 1500) {
        int i, ph = (ms / 150) % 6;
        for (i = 0; i < 3; i++) {
            int on = (ph == i || ph == i + 1);
            gfx_circle(SCR_W/2 - 16 + i * 16, 362, on ? 5 : 4, on ? 0xFFFFFF : 0x6A2FD0, 255);
        }
        gfx_text_c(&font_small, SCR_W/2, 380, net_done ? (net_res ? "Conectado" : "Sin red") : "Conectando...", 0xE6DAFF);
    }
    if (ms > 1800) {
        gfx_text_c(&font_small, SCR_W/2, 424, "M\xc3\xbasica: Debussy, R\xc3\xaaverie - David Hernando Vitores (CC BY-SA 4.0)", 0xD9C7FF);
        gfx_text_c(&font_small, SCR_W/2, 442, "App no oficial hecha por fans - no afiliada a Twitch ni a Nintendo", 0xD9C7FF);
        gfx_text(&font_small, SCR_W - 16 - gfx_text_w(&font_small, "A: saltar  B: silencio"), 18, "A: saltar  B: silencio", 0xD9C7FF);
    }
    if (ms < 400) fade_black(255 - ms * 255 / 400);
    if (out_ms >= 0) fade_black(out_ms * 255 / 300);
}

/* devuelve el resultado de sh_net_ready() */
int intro_run(void) {
    net_done = 0;
    if (LWP_CreateThread(&net_th, net_fn, NULL, NULL, 64 * 1024, 60) < 0) { net_th = LWP_THREAD_NULL; net_res = sh_net_ready(); net_done = 1; }
    music_start();
    u64 t0 = gettime(), tout = 0;
    int skip = 0;
    while (SYS_MainLoop()) {
        int ms = (int)ticks_to_millisecs(gettime() - t0);
        u32 d = in_poll();
        while (in_key()) d |= WPAD_BUTTON_A;
        if (!skip && ms > 300 && (d & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_2 | WPAD_BUTTON_PLUS | WPAD_BUTTON_HOME))) {
            skip = 1;
            if (d & WPAD_BUTTON_B) intro_music_stop();
        }
        int ready = (net_done || ms > MAX_NET_MS) && (skip || ms >= MIN_MS);
        if (ready && !tout) tout = gettime();
        int out_ms = tout ? (int)ticks_to_millisecs(gettime() - tout) : -1;
        draw(ms, out_ms);
        gfx_present();
        if (out_ms >= 300) break;
    }
    if (net_th != LWP_THREAD_NULL) {
        if (!net_done) { sui_busy("TwitchWii", "Iniciando la red..."); while (!net_done) { ui_busy_tick(); VIDEO_WaitVSync(); } sui_busy_end(); }
        LWP_JoinThread(net_th, NULL); net_th = LWP_THREAD_NULL;
    }
    return net_res;
}
