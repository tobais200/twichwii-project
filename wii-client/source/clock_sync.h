#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H
#include <stdint.h>
/* Ten minutes accommodates small RTC errors before a verified HTTPS Date arrives. */
#define SH_CLOCK_GRACE 600
int64_t sh_clock_now(void);
int sh_clock_synced(void);
int sh_clock_accept(int64_t server_epoch);
int sh_clock_expired(int64_t expiry);
int64_t sh_clock_epoch(int y,int m,int d,int hh,int mm,int ss);
void sh_clock_attach(void *curl);
#endif
