/* dlog.h - registro de diagnostico en sd:/apps/TwitchWii/log.txt .
   Cualquier hilo puede llamar dlog(); solo el hilo principal escribe a la SD (dlog_flush). */
#ifndef DLOG_H
#define DLOG_H
void dlog_init(int sd_ok);                 /* rota log.txt -> log-anterior.txt y abre uno nuevo */
void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dlog_flush(int force);                /* hilo principal: vuelca el buffer cada ~2 s */
int  dlog_enabled(void);
void dlog_host(const char *url, char *out, int sz);   /* solo el host (nunca el token de la URL) */
#endif
