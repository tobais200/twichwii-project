/*
 * dlog.c - registro de diagnostico para la Wii real (TwitchWii, copiado de KickWii).
 * Escribe sd:/apps/TwitchWii/log.txt (la sesion anterior queda en log-anterior.txt).
 * Se desactiva creando sd:/apps/TwitchWii/nolog.txt. Tope: 1 MB por sesion.
 * Nunca se registran tokens: las URLs se reducen al host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include "dlog.h"

int fs_dev_mounted(const char *path);
static char dlog_dir[40] = "sd:/apps/TwitchWii/";
#define DLOG_DIR dlog_dir
#define DLOG_BUF   (24 * 1024)
#define DLOG_MAX   (1024 * 1024)
static char buf[DLOG_BUF];
static int blen = 0, on = 0, mtx_ok = 0, dropped = 0;
static long written = 0;
static mutex_t m;
static u64 t0 = 0, last_flush = 0;

int dlog_enabled(void) { return on; }

void dlog_host(const char *url, char *out, int sz) {
    const char *h = url ? strstr(url, "://") : NULL;
    h = h ? h + 3 : (url ? url : "");
    int n = (int)strcspn(h, "/?#");
    if (n >= sz) n = sz - 1;
    memcpy(out, h, n); out[n] = 0;
}

void dlog_init(int sd_ok) {
    struct stat st;
    if (!mtx_ok) { LWP_MutexInit(&m, false); mtx_ok = 1; }
    t0 = gettime();
    on = 0;
    char p[80], p2[80];
    if (!sd_ok) return;
    /* fatInitDefault() es verdadero aunque solo monte "usb:"; antes se
     * hacia mkdir("sd:/apps") igual y libsysbase leia devoptab_list[-1] (NULL)
     * -> excepcion DSI al arrancar en la Wii real. Ahora se usa solo un
     * dispositivo realmente montado. */
    if (fs_dev_mounted("sd:/")) { snprintf(dlog_dir, sizeof(dlog_dir), "sd:/apps/TwitchWii/"); mkdir("sd:/apps", 0777); }
    else if (fs_dev_mounted("usb:/")) { snprintf(dlog_dir, sizeof(dlog_dir), "usb:/apps/TwitchWii/"); mkdir("usb:/apps", 0777); }
    else return;
    snprintf(p, sizeof(p), "%snolog.txt", dlog_dir);
    if (stat(p, &st) == 0) return;
    snprintf(p, sizeof(p), "%.*s", (int)strlen(dlog_dir) - 1, dlog_dir);
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%slog-anterior.txt", dlog_dir);
    remove(p);
    snprintf(p2, sizeof(p2), "%slog.txt", dlog_dir);
    rename(p2, p);
    FILE *f = fopen(p2, "w");
    if (!f) return;
    fclose(f);
    on = 1; written = 0;
}

#ifdef DLOG_UDP
/* solo pruebas en Dolphin: copia cada linea por UDP a 127.0.0.1:9977 (la SD de Dolphin solo se sincroniza al cerrar) */
#include <sys/socket.h>
#include <netinet/in.h>
static void udp_line(const char *l, int n) {
    static int fd = -1; struct sockaddr_in a;
    if (fd < 0) fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = 9977; a.sin_addr.s_addr = 0x7f000001;
    sendto(fd, l, n, 0, (struct sockaddr *)&a, sizeof a);
}
#endif
void dlog(const char *fmt, ...) {
    char line[400];
    va_list ap;
    if (!on || !mtx_ok) return;
    u32 ms = ticks_to_millisecs(gettime() - t0);
    int n = snprintf(line, sizeof(line), "[%5lu.%01lu] ", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    n = strlen(line);
    line[n++] = '\n'; line[n] = 0;
#ifdef DLOG_UDP
    udp_line(line, n);
#endif
    LWP_MutexLock(m);
    if (blen + n < DLOG_BUF) { memcpy(buf + blen, line, n); blen += n; }
    else dropped++;
    LWP_MutexUnlock(m);
}

void dlog_flush(int force) {
    static char out[DLOG_BUF];
    int n, dr;
    if (!on) return;
    u64 now = gettime();
    if (!force && ticks_to_millisecs(now - last_flush) < 2000) return;
    last_flush = now;
    LWP_MutexLock(m);
    n = blen; dr = dropped;
    if (n) memcpy(out, buf, n);
    blen = 0; dropped = 0;
    LWP_MutexUnlock(m);
    if (!n && !dr) return;
    if (written > DLOG_MAX) return;
    char lp[80]; snprintf(lp, sizeof(lp), "%slog.txt", dlog_dir);
    FILE *f = fopen(lp, "a");
    if (!f) return;
    if (n) fwrite(out, 1, n, f);
    if (dr) fprintf(f, "(%d lineas perdidas: buffer lleno)\n", dr);
    written += n;
    if (written > DLOG_MAX) fputs("(log.txt llego a 1 MB: no se escribe mas en esta sesion)\n", f);
    fclose(f);
}
