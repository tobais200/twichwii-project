/* rc4b: authenticated HTTPS Date clock, not a change to the Wii system RTC.
 * Chain/signature/hostname verification stays enabled. Only certificate validity
 * dates are recomputed with corrected UTC and a bounded ten-minute grace. */
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include "clock_sync.h"
#ifndef SH_CLOCK_HOST
#include <gccore.h>
#include <ogc/irq.h>
#include <ogc/lwp_watchdog.h>
static uint64_t mono_seconds(void) { return ticks_to_secs(gettime()); }
static unsigned lock_clock(void) { return IRQ_Disable(); }
static void unlock_clock(unsigned l) { IRQ_Restore(l); }
#else
static uint64_t mono_seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec; }
static unsigned lock_clock(void) { return 0; }
static void unlock_clock(unsigned l) { (void)l; }
#endif
static int synchronized;
static int64_t anchor_epoch;
static uint64_t anchor_mono;
int64_t sh_clock_epoch(int y,int m,int d,int hh,int mm,int ss) {
    /* Gregorian civil date to UTC; no local timezone/mktime dependence. */
    y -= m <= 2;
    int era = (y >= 0 ? y : y-399) / 400;
    unsigned yo = (unsigned)(y-era*400);
    unsigned doy = (153*(m+(m>2?-3:9))+2)/5+d-1;
    unsigned doe = yo*365+yo/4-yo/100+doy;
    return ((int64_t)era*146097+doe-719468)*86400+hh*3600+mm*60+ss;
}
int sh_clock_synced(void) { unsigned l=lock_clock(); int s=synchronized; unlock_clock(l); return s; }
int64_t sh_clock_now(void) {
    uint64_t mono=mono_seconds();
    unsigned l=lock_clock(); int s=synchronized; int64_t epoch=anchor_epoch; uint64_t tick=anchor_mono; unlock_clock(l);
    return s ? epoch+(int64_t)(mono-tick) : (int64_t)time(NULL);
}
int sh_clock_accept(int64_t server_epoch) {
    /* Ignore malformed, stale/cached (>24h RTC drift) or non-contemporary dates.
       The initial TLS connection must already pass all checks plus bounded grace. */
    int64_t rtc=(int64_t)time(NULL);
    if(server_epoch<1577836800LL || llabs(server_epoch-rtc)>86400) return 0;
    uint64_t mono=mono_seconds(); unsigned l=lock_clock();
    if(!synchronized) { anchor_epoch=server_epoch; anchor_mono=mono; synchronized=1; }
    unlock_clock(l); return 1;
}
int sh_clock_expired(int64_t expiry) { return sh_clock_now()>expiry+SH_CLOCK_GRACE; }
static size_t clock_header(char *data,size_t a,size_t b,void *opaque) {
    size_t n=a*b; CURL *curl=(CURL *)opaque; char *url=NULL;
    if(n>6 && n<128 && !strncasecmp(data,"Date:",5) && !sh_clock_synced()) {
        curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&url);
        /* Never trust LAN/plain HTTP for certificate time. Header callbacks run
           after the TLS handshake and certificate/hostname validation. */
        if(url && !strncasecmp(url,"https://",8)) {
            char date[128]; memcpy(date,data+5,n-5);date[n-5]=0;
            time_t t=curl_getdate(date,NULL); if(t!=(time_t)-1) sh_clock_accept((int64_t)t);
        }
    }
    return n;
}
static int clock_verify(void *ctx,mbedtls_x509_crt *crt,int depth,uint32_t *flags) {
    (void)ctx;(void)depth;
    int64_t now=sh_clock_now();
    const mbedtls_x509_time *f=&crt->valid_from,*t=&crt->valid_to;
    int64_t from=sh_clock_epoch(f->year,f->mon,f->day,f->hour,f->min,f->sec);
    int64_t to=sh_clock_epoch(t->year,t->mon,t->day,t->hour,t->min,t->sec);
    /* Preserve EVERY non-time failure, including CN mismatch, untrusted issuer,
       signatures, revocation, key/algorithm policy. Re-add time flags if needed. */
    *flags &= ~(MBEDTLS_X509_BADCERT_FUTURE|MBEDTLS_X509_BADCERT_EXPIRED);
    if(now+SH_CLOCK_GRACE<from) *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if(now-SH_CLOCK_GRACE>to) *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    return 0;
}
static CURLcode clock_ctx(CURL *c,void *config,void *opaque) {
    (void)c;(void)opaque;
    mbedtls_ssl_conf_verify((mbedtls_ssl_config *)config,clock_verify,NULL);
    return CURLE_OK;
}
void sh_clock_attach(void *c) {
    CURL *curl=(CURL *)c;
    curl_easy_setopt(curl,CURLOPT_SSL_CTX_FUNCTION,clock_ctx);
    curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,clock_header);
    curl_easy_setopt(curl,CURLOPT_HEADERDATA,curl);
    /* No cached sessions bypassing the validity callback after a clock update. */
    curl_easy_setopt(curl,CURLOPT_SSL_SESSIONID_CACHE,0L);
}
