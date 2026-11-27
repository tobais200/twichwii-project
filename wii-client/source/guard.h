/* guard.h - vigilante del hilo principal , ver guard.c */
#ifndef GUARD_H
#define GUARD_H
#include <gccore.h>
void guard_init(void);                 /* desde main(), despues de WPAD_Init y dlog_init */
void guard_beat(void);                 /* latido: solo cuenta si lo llama el hilo principal */
void guard_where(const char *w);       /* donde esta el hilo principal (para el log) */
const char *guard_get_where(void);
void guard_curl(void *curl);           /* sockets de curl registrados + latido en el progreso */
void guard_sock_add(int fd);
void guard_sock_del(int fd);
int  guard_kill_socks(lwp_t owner);    /* LWP_THREAD_NULL = todos */
void guard_test_hang(int on);          /* solo con -DGUARD_TEST */
u32  guard_stall_count(void);
#endif
