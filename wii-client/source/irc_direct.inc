/* Anonymous read fallback; personal OAuth only over encrypted TLS/WSS. */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>
#include "chat_net.h"
typedef struct {int fd,welcomed;chat_net *net;const char *label;} irc_conn;
static int irc_send(irc_conn *c,const char *s) {
 if(c->net) {
  if(!c->net->ws)return cn_send(c->net,s);
  /* Twitch WS expects ONE IRC command per WebSocket message. */
  char line[512];const char *p=s;
  while(*p){int n=strcspn(p,"\r\n");if(n){if(n>=(int)sizeof(line))return 0;memcpy(line,p,n);line[n]=0;if(!cn_send(c->net,line))return 0;}p+=n;while(*p=='\r'||*p=='\n')p++;}
  return 1;
 }
 size_t off=0,len=strlen(s);u64 start=gettime();
 while(run&&off<len&&ticks_to_millisecs(gettime()-start)<4000) {
  struct pollfd p={c->fd,POLLOUT,0};if(poll(&p,1,100)<=0)continue;
  int n=send(c->fd,s+off,len-off,0);if(n<=0)return 0;off+=n;
 }return off==len;
}
static void irc_tag(const char *line, const char *key, char *out, int sz) {
    out[0] = 0;
    if (line[0] != '@') return;
    const char *p = line + 1;
    size_t k = strlen(key);
    while (*p && *p != ' ') {
        const char *end = strpbrk(p, "; ");
        if (!end) return;
        if ((size_t)(end - p) > k && !strncmp(p, key, k) && p[k] == '=') {
            int i = 0; p += k + 1;
            while (p < end && i < sz - 1) {
                char c = *p++;
                if (c == '\\' && p < end) { c = *p++; if (c == 's') c = ' '; else if (c == ':') c = ';'; else if (c == 'r' || c == 'n') c = ' '; }
                out[i++] = c;
            }
            out[i] = 0; return;
        }
        p = *end == ';' ? end + 1 : end;
    }
}
static int irc_line(irc_conn *c, char *line) {
    if (!strncmp(line, "HTTP/", 5)) { snprintf(status,sizeof(status),"%s",T("IRC TCP rechazado; probando TLS/WSS...")); return 0; }
    if (!strncmp(line, "PING ", 5)) { char pong[4104]; snprintf(pong, sizeof(pong), "PONG %s\r\n", line + 5); return irc_send(c, pong); }
    if (strstr(line, " RECONNECT")) return 0;
    if(strstr(line," 001 ")) {c->welcomed=1;snprintf(status,sizeof(status),T("IRC %s: conectado"),c->label);}
    if (strstr(line, "NOTICE") && (strstr(line, "authentication failed") || strstr(line, "Login unsuccessful"))) {
        LWP_MutexLock(mtx);rejected=1;write_ready=0;outgoing[0]=0;LWP_MutexUnlock(mtx);
        snprintf(status, sizeof(status), T("Twitch rechazo la autenticacion. Inicia sesion otra vez.")); run = 0; return 0;
    }
    /* Respect moderator deletion without retaining deleted content. */
    if (strstr(line, " CLEARCHAT ") || strstr(line, " CLEARMSG ")) {
        LWP_MutexLock(mtx); head = count = 0; gen++; LWP_MutexUnlock(mtx);
    }
    if (strstr(line," NOTICE ")) { const char *reason=strstr(line," :"); if(reason)push("Twitch",reason+2,0,1); }
    char *cmd = strstr(line, " PRIVMSG #");
    if (!cmd) return 1;
    char expected[60]; snprintf(expected, sizeof(expected), " PRIVMSG #%s :", login);
    if (strncmp(cmd, expected, strlen(expected))) return 1;
    char *body = cmd + strlen(expected), name[28] = "", color[12] = "";
    irc_tag(line, "display-name", name, sizeof(name)); irc_tag(line, "color", color, sizeof(color));
    if (!name[0]) {
        const char *prefix = line[0] == '@' ? strchr(line, ' ') : line;
        if (prefix) { while (*prefix == ' ' || *prefix == ':') prefix++; int i = 0; while (prefix[i] && prefix[i] != '!' && i < 27) { name[i] = prefix[i]; i++; } name[i] = 0; }
    }
    if (!strncmp(body, "\001ACTION ", 8)) { body += 8; char *end = strchr(body, '\001'); if (end) *end = 0; }
    { static char emotes[600]; irc_tag(line, "emotes", emotes, sizeof(emotes)); push_e(name, body, parse_color(color), 0, emotes); }
    return 1;
}
static int irc_connect(void) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("irc.chat.twitch.tv", "6667", &hints, &res)) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    guard_sock_add(fd);   /* el vigilante lo puede cortar */
    unsigned long nonblock = 1;
    if (ioctl(fd, FIONBIO, &nonblock) < 0) { guard_sock_del(fd); close(fd); freeaddrinfo(res); return -1; }
    int rc = connect(fd, res->ai_addr, res->ai_addrlen); freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) { guard_sock_del(fd); close(fd); return -1; }
    int i, ready = rc == 0;
    for (i = 0; !ready && run && i < 32; i++) {
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, 250) > 0) { int error = 0; socklen_t n = sizeof(error); ready = !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) && !error; break; }
    }
    if (!ready || !run) { guard_sock_del(fd); close(fd); return -1; }
    return fd;
}
static void *direct_thread(void *unused) {
 (void)unused;int mode=authenticated?1:0,cycles=0;
#ifdef AUTH_MOCK
 mode=1;
#endif
 chat_net *net=calloc(1,sizeof(*net));if(!net)return NULL;
 static const char *labels[]={"TCP 6667","TLS 6697","WSS 443","WS 80"};
 while(run) {
  irc_conn c={.fd=-1,.label=labels[mode]};
  snprintf(status,sizeof(status),T("Conectando IRC %s..."),c.label);
  int ok=0;
  if(mode==0){c.fd=irc_connect();ok=c.fd>=0;}
  else {
   const char *url=mode==1?"https://irc.chat.twitch.tv:6697/":mode==2?"wss://irc-ws.chat.twitch.tv:443/":"ws://irc-ws.chat.twitch.tv:80/";
#ifdef AUTH_MOCK
   url="https://127.0.0.1:8991/";
#endif
   ok=cn_open(net,url,mode>=2,"https://www.twitch.tv",&run);if(ok)c.net=net;
  }
  if(ok) {
   char intro[2300];
   if(authenticated)snprintf(intro,sizeof(intro),"CAP REQ :twitch.tv/tags twitch.tv/commands\r\nPASS oauth:%s\r\nNICK %s\r\nJOIN #%s\r\n",authpass,authnick,login);
   else snprintf(intro,sizeof(intro),"CAP REQ :twitch.tv/tags twitch.tv/commands\r\nPASS SCHMOOPIIE\r\nNICK justinfan%d\r\nJOIN #%s\r\n",10000+rand()%89999,login);
   int alive=irc_send(&c,intro),n=0,overflow=0;memset(intro,0,sizeof(intro));char line[4096],buf[2048];u64 begin=gettime(),last=begin,ping=begin;
   while(run&&alive) {
    int got=0;const char *data=buf;
    if(c.net) {
     if(c.net->ws){int rc=cn_message(c.net);if(rc<0)break;if(rc>0){data=c.net->msg;got=strlen(data);}}
     else {size_t nr=0;CURLcode rc=curl_easy_recv(c.net->c,buf,sizeof(buf),&nr);if(rc!=CURLE_OK&&rc!=CURLE_AGAIN)break;if(rc==CURLE_OK&&!nr)break;got=nr;}
    }else {
     struct pollfd pollfd={c.fd,POLLIN,0};int rc=poll(&pollfd,1,100);
     if(rc<0||(pollfd.revents&(POLLERR|POLLHUP|POLLNVAL)))break;
     if(rc>0){got=recv(c.fd,buf,sizeof(buf),0);if(got<=0)break;}
    }
    if(got>0){last=gettime();int i;for(i=0;i<got&&alive;i++) {
     char ch=data[i];if(ch=='\n'){if(!overflow){if(n&&line[n-1]=='\r')n--;line[n]=0;alive=irc_line(&c,line);}n=overflow=0;}
     else if(!overflow){if(n<(int)sizeof(line)-1)line[n++]=ch;else overflow=1;}
    }}else usleep(20000);
    LWP_MutexLock(mtx);write_ready=run&&authenticated&&c.welcomed;LWP_MutexUnlock(mtx);
    if(write_ready) {
     char text[351]="",command[440];static char etag[600];etag[0]=0;LWP_MutexLock(mtx);if(outgoing[0]){snprintf(text,sizeof(text),"%s",outgoing);snprintf(etag,sizeof(etag),"%s",outgoing_tag);outgoing[0]=0;outgoing_tag[0]=0;}LWP_MutexUnlock(mtx);
     if(text[0]){snprintf(command,sizeof(command),"PRIVMSG #%s :%s\r\n",login,text);
      /* Clear before I/O: an uncertain/disconnected send is NEVER replayed. */
      if(!irc_send(&c,command)){push(T("Sistema"),T("Envio incierto: no se reintentara automaticamente"),0,1);alive=0;}
      else {push_e(authnick,text,C_ACCENT,0,etag);snprintf(status,sizeof(status),T("Mensaje enviado a IRC (sujeto a moderacion)"));}
      memset(text,0,sizeof(text));memset(command,0,sizeof(command));
     }
    }
    u64 now=gettime();if(!c.welcomed&&ticks_to_millisecs(now-begin)>10000)break;
    if(ticks_to_millisecs(now-last)>90000)break;
    if(ticks_to_millisecs(now-ping)>30000){if(!irc_send(&c,"PING :wii\r\n"))break;ping=now;}
   }
   LWP_MutexLock(mtx);write_ready=0;int dropped=outgoing[0]!=0;outgoing[0]=0;LWP_MutexUnlock(mtx);
   if(dropped)push(T("Sistema"),T("Chat desconectado: mensaje descartado; no se reenvia"),0,1);
   if(c.fd>=0){guard_sock_del(c.fd);close(c.fd);}
   cn_close(net);
  }
  if(!run)break;
  if(!c.welcomed){mode=authenticated?(mode==1?2:1):(mode+1)%4;cycles++;
#ifdef AUTH_MOCK
 mode=1;
#endif
  }else cycles=0;
  snprintf(status,sizeof(status),T("Reconectando IRC: %s"),labels[mode]);
  int i,wait=cycles>=4?150:10;for(i=0;i<wait&&run;i++)usleep(100000);
 }
 cn_close(net);free(net);return NULL;
}
