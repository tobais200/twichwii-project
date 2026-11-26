/*
 * guard.c - vigilante del hilo principal (KickWii 1.3-rc16 / TwichWii 1.2-rc11).
 *
 * Tobias vio en la Wii real: despues de un rato mirando un directo la imagen queda congelada,
 * el audio sigue y ningun boton responde (habia que apagar la Wii). Eso es el hilo principal
 * (dibujo + botones) bloqueado mientras los hilos de audio siguen: un LWP_JoinThread de un hilo
 * de chat/avisos trabado en un socket de IOS, un pedido HTTP colgado en el hilo principal, o
 * hilos de video de mayor prioridad que no lo dejan correr.
 *
 * Este hilo (prioridad 120, duerme 100 ms) mira un "latido" que el hilo principal da en
 * in_poll()/gfx_present()/ui_busy_tick()/progreso de curl. Si se detiene:
 *   >= 2 s : lee el Wiimote/mando por su cuenta: HOME (o RESET de la consola) = salida de emergencia
 *   >= 4 s : lo anota en el log (donde quedo trabado) y baja la prioridad de los hilos de video
 *   >= 8 s : corta (shutdown) los sockets de chat/avisos/miniaturas/pedidos del hilo principal
 *   >= 12 s: corta tambien los del video (el vigilante del reproductor reconecta)
 *   cada 6 s mas: repite el corte de los sockets nuevos de chat/pedidos (nunca dos veces el mismo).
 * Cuando el hilo principal vuelve, restaura las prioridades y anota cuanto duro.
 * El boton POWER (consola o Wiimote) apaga la Wii en cualquier momento.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <wiiuse/wpad.h>
#include <sys/socket.h>
#include "dlog.h"
#include "guard.h"

static lwp_t g_main = LWP_THREAD_NULL, g_th = LWP_THREAD_NULL, g_fl = LWP_THREAD_NULL;
static volatile u32 g_hb = 0;
static volatile const char *g_where = "inicio";
static volatile int g_power = 0, g_reset = 0, g_flush_req = 0;
static volatile int g_test_hang = 0;
static volatile u32 g_stalls = 0, g_kills = 0;

/* ---- registro de sockets abiertos por curl (todos los hilos salvo el video, que tiene el suyo) */
#define GSOCK 24
static volatile int gs_fd[GSOCK];
static volatile lwp_t gs_own[GSOCK];
static volatile u8 gs_dead[GSOCK];  /* ya cortado: no repetir shutdown sobre el mismo fd */
static void gs_init(void) { for (int i = 0; i < GSOCK; i++) { gs_fd[i] = -1; gs_own[i] = LWP_THREAD_NULL; } }
void guard_sock_add(int fd) {
    if (fd < 0) return;
    u32 l; _CPU_ISR_Disable(l);
    for (int i = 0; i < GSOCK; i++) if (gs_fd[i] < 0) { gs_fd[i] = fd; gs_own[i] = LWP_GetSelf(); gs_dead[i] = 0; break; }
    _CPU_ISR_Restore(l);
}
void guard_sock_del(int fd) {
    u32 l; _CPU_ISR_Disable(l);
    for (int i = 0; i < GSOCK; i++) if (gs_fd[i] == fd) { gs_fd[i] = -1; gs_own[i] = LWP_THREAD_NULL; gs_dead[i] = 0; }
    _CPU_ISR_Restore(l);
}
/* owner == LWP_THREAD_NULL: todos */
int guard_kill_socks(lwp_t owner) {
    int n = 0;
    for (int i = 0; i < GSOCK; i++) {
        int fd = gs_fd[i];
        if (fd >= 0 && !gs_dead[i] && (owner == LWP_THREAD_NULL || gs_own[i] == owner)) {
#ifdef DLOG_UDP
            dlog("PRUEBA: shutdown fd %d (slot %d)", fd, i);
#endif
            gs_dead[i] = 1; shutdown(fd, SHUT_RDWR); n++;
        }
    }
    g_kills += n;
    return n;
}
#include <curl/curl.h>
static curl_socket_t g_open(void *cl, curlsocktype purpose, struct curl_sockaddr *a) {
    (void)cl; (void)purpose;
    int fd = socket(a->family, a->socktype, a->protocol);
    if (fd >= 0) guard_sock_add(fd);
    return fd;
}
static int g_close(void *cl, curl_socket_t fd) { (void)cl; guard_sock_del(fd); return close(fd); }
static int g_prog(void *c, curl_off_t a, curl_off_t b, curl_off_t d, curl_off_t e) { (void)c; (void)a; (void)b; (void)d; (void)e; guard_beat(); return 0; }
void guard_curl(void *curl) {
    curl_easy_setopt((CURL *)curl, CURLOPT_OPENSOCKETFUNCTION, g_open);
    curl_easy_setopt((CURL *)curl, CURLOPT_CLOSESOCKETFUNCTION, g_close);
    /* latido mientras un pedido del hilo principal avanza (los que ponen su propio progreso lo reemplazan) */
    curl_easy_setopt((CURL *)curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt((CURL *)curl, CURLOPT_XFERINFOFUNCTION, g_prog);
}

/* ---- latido */
void guard_beat(void) {
    if (g_main == LWP_THREAD_NULL || LWP_GetSelf() != g_main) return;
#ifdef GUARD_TEST
    if (g_test_hang) { g_where = "PRUEBA: bloqueo simulado";
#ifdef GUARD_TEST_RESUME
        u64 h0 = gettime(); while (g_test_hang && ticks_to_millisecs(gettime() - h0) < 16000) usleep(50000); g_test_hang = 0; g_where = "PRUEBA: fin del bloqueo";
#else
        while (g_test_hang) usleep(50000);
#endif
    }
#endif
    g_hb++;
}
void guard_where(const char *w) { if (LWP_GetSelf() == g_main) g_where = w; }
const char *guard_get_where(void) { return (const char *)g_where; }
void guard_test_hang(int on) { g_test_hang = on; }
u32 guard_stall_count(void) { return g_stalls; }

/* ---- botones de la consola / Wiimote */
static void on_power(void) { g_power = 1; }
static void on_wpad_power(s32 chan) { (void)chan; g_power = 1; }
static void on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; g_reset = 1; }

/* ---- el log se escribe desde un hilo aparte: el vigilante nunca toca la SD */
static void *flusher(void *a) {
    (void)a;
    while (1) { if (g_flush_req) { g_flush_req = 0; dlog_flush(1); } usleep(200000); }
    return NULL;
}

__attribute__((weak)) void tp_guard(int level) { (void)level; }
__attribute__((weak)) void tp_guard_info(char *o, int n) { if (n) o[0] = 0; }

void gfx_black_now(void);
static void emergency(const char *why) {
    dlog("VIGILANTE UI: salida de emergencia (%s) trabado en '%s'", why, (const char *)g_where);
    g_flush_req = 1;
    for (int i = 0; i < 6 && g_flush_req; i++) usleep(100000);
    guard_kill_socks(LWP_THREAD_NULL);
    tp_guard(2);
    gfx_black_now();
    _exit(0);   /* sin atexit: vuelve al Homebrew Channel (stub) o al menu de la Wii */
}

static void *guard_thread(void *a) {
    (void)a;
    u32 last = g_hb; u64 last_t = gettime(), next_kill = 0;
    int stage = 0;
    while (1) {
        usleep(100000);
        if (g_power) {
            dlog("VIGILANTE UI: boton POWER: apagando");
            g_flush_req = 1; for (int i = 0; i < 6 && g_flush_req; i++) usleep(100000);
            SYS_ResetSystem(SYS_POWEROFF, 0, 0);
        }
        if (g_reset) emergency("RESET");
        u32 hb = g_hb; u64 now = gettime();
        if (hb != last) {
            if (stage) {
                dlog("VIGILANTE UI: el hilo principal volvio despues de %u ms (estaba en '%s')",
                     (unsigned)ticks_to_millisecs(now - last_t), (const char *)g_where);
                tp_guard(0);
                g_flush_req = 1;
            }
            last = hb; last_t = now; stage = 0;
            continue;
        }
        u32 ms = ticks_to_millisecs(now - last_t);
        if (ms >= 2000) {
            WPAD_ScanPads(); PAD_ScanPads();
            u32 w = WPAD_ButtonsDown(0) | WPAD_ButtonsHeld(0);
            if ((w & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) || (PAD_ButtonsHeld(0) & PAD_BUTTON_START) ) emergency("HOME");
        }
        if (ms >= 4000 && stage < 1) {
            char ti[200]; tp_guard_info(ti, sizeof ti);
            stage = 1; g_stalls++;
            dlog("VIGILANTE UI: hilo principal sin responder hace %u ms en '%s' | %s", (unsigned)ms, (const char *)g_where, ti);
            tp_guard(1);
            g_flush_req = 1;
        }
        if (ms >= 8000 && stage < 2) {
            stage = 2; next_kill = now + secs_to_ticks(6);
            int n = guard_kill_socks(LWP_THREAD_NULL);
            dlog("VIGILANTE UI: corto %d socket(s) de chat/miniaturas/pedidos (sigue en '%s')", n, (const char *)g_where);
            g_flush_req = 1;
        }
        if (ms >= 12000 && stage < 3) {
            stage = 3; tp_guard(3);
            dlog("VIGILANTE UI: corto tambien los sockets del video");
            g_flush_req = 1;
        }
        if (stage >= 2 && now >= next_kill) {
            next_kill = now + secs_to_ticks(6);
#ifdef DLOG_UDP
            dlog("PRUEBA: repito cortes");
#endif
            guard_kill_socks(LWP_THREAD_NULL);
            /* el video se corta una sola vez (etapa 3): al volver el hilo principal su vigilante reabre el stream */
#ifdef DLOG_UDP
            dlog("PRUEBA: cortes repetidos ok");
#endif
        }
    }
    return NULL;
}

void guard_init(void) {
    g_main = LWP_GetSelf();
    gs_init();
    SYS_SetPowerCallback(on_power);
    WPAD_SetPowerButtonCallback(on_wpad_power);
    SYS_SetResetCallback(on_reset);
    if (LWP_CreateThread(&g_fl, flusher, NULL, NULL, 32 * 1024, 40) < 0) g_fl = LWP_THREAD_NULL;
    if (LWP_CreateThread(&g_th, guard_thread, NULL, NULL, 32 * 1024, 120) < 0) g_th = LWP_THREAD_NULL;
}
