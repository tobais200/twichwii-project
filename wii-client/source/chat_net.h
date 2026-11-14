#ifndef CHAT_NET_H
#define CHAT_NET_H
#include <curl/curl.h>
#include <curl/websockets.h>
#include <ogc/lwp_watchdog.h>
#define CHAT_WSMAX 32768
typedef struct { CURL *c;int ws,len,drop;volatile int *running;char err[CURL_ERROR_SIZE];char msg[CHAT_WSMAX]; } chat_net;
static int cn_progress(void *v,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
 (void)a;(void)b;(void)c;(void)d;return !*((chat_net*)v)->running;
}
static void cn_close(chat_net *n){if(n->c)curl_easy_cleanup(n->c);n->c=NULL;n->len=n->drop=0;}
static int cn_open(chat_net *n,const char *url,int ws,const char *origin,volatile int *running) {
 memset(n,0,sizeof(*n));n->running=running;n->ws=ws;n->c=curl_easy_init();if(!n->c)return 0;
 sh_curl_common(n->c,n->err);
 curl_easy_setopt(n->c,CURLOPT_PROTOCOLS_STR,"HTTP,HTTPS,WS,WSS");
 curl_easy_setopt(n->c,CURLOPT_REDIR_PROTOCOLS_STR,"HTTPS,WSS");
 curl_easy_setopt(n->c,CURLOPT_URL,url);curl_easy_setopt(n->c,CURLOPT_CONNECT_ONLY,ws?2L:1L);
 curl_easy_setopt(n->c,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(n->c,CURLOPT_TIMEOUT,7L);
 curl_easy_setopt(n->c,CURLOPT_BUFFERSIZE,16384L);curl_easy_setopt(n->c,CURLOPT_NOSIGNAL,1L);
 curl_easy_setopt(n->c,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(n->c,CURLOPT_XFERINFOFUNCTION,cn_progress);curl_easy_setopt(n->c,CURLOPT_XFERINFODATA,n);
 struct curl_slist *h=NULL;if(origin){char o[128];snprintf(o,sizeof(o),"Origin: %s",origin);h=curl_slist_append(h,o);curl_easy_setopt(n->c,CURLOPT_HTTPHEADER,h);}
 CURLcode rc=curl_easy_perform(n->c);curl_slist_free_all(h);curl_easy_setopt(n->c,CURLOPT_HTTPHEADER,NULL);
 if(rc!=CURLE_OK){snprintf(n->err,sizeof(n->err),"curl %d: %s",rc,curl_easy_strerror(rc));cn_close(n);return 0;}return 1;
}
static int cn_send(chat_net *n,const char *s) {
 size_t len=strlen(s),off=0;u64 start=gettime();
 while(*n->running&&off<len&&ticks_to_millisecs(gettime()-start)<4000) {
  size_t sent=0;CURLcode rc=n->ws?curl_ws_send(n->c,s+off,len-off,&sent,0,CURLWS_TEXT):curl_easy_send(n->c,s+off,len-off,&sent);
  if(rc==CURLE_AGAIN){usleep(10000);continue;}if(rc!=CURLE_OK)return 0;off+=sent;
 }return off==len;
}
/* 1 = complete text frame/message; 0 = nonblocking wait/control frame; -1 = closed/error.
   libcurl handles TLS, RFC6455 masking, partial frames, ping/pong and certificate checks. */
static int cn_message(chat_net *n) {
 char buf[2048];size_t got=0;const struct curl_ws_frame *m=NULL;
 CURLcode rc=curl_ws_recv(n->c,buf,sizeof(buf),&got,&m);
 if(rc==CURLE_AGAIN)return 0;
 if(rc!=CURLE_OK||!m)return -1;
 if(m->flags&CURLWS_CLOSE)return -1;
 if(m->flags&(CURLWS_PING|CURLWS_PONG|CURLWS_BINARY))return 0;
 if(got>0){if(n->len+(int)got>=CHAT_WSMAX)n->drop=1;if(!n->drop){memcpy(n->msg+n->len,buf,got);n->len+=got;}}
 if(m->bytesleft==0&&!(m->flags&CURLWS_CONT)) {
  n->msg[n->len]=0;if(n->drop){n->len=n->drop=0;return 0;}n->len=0;return 1;
 }return 0;
}
#endif
