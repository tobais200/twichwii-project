#define WII_TWITCH_AUTH
/* Public QR login: Twitch Public Device Grant / publisher Kick OAuth relay.
 * No Wii client secret or manual PKCE callback. Private tokens live on SD.
 * AUTH_MOCK is compile-only; never an INI-configurable HTTP endpoint. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#ifdef AUTH_HOST_TEST
#include "auth_test_shim.h"
#else
#include "tv.h"
#endif
#include "auth.h"
#include "oauth_config.h"
#ifndef AUTH_HOST_TEST
#include <ogc/lwp_watchdog.h>
#endif
#ifdef WII_TWITCH_AUTH
#define SITE "Twitch"
#define SCOPES "user:read:chat user:write:chat chat:read chat:edit user:read:follows"
#define TOKEN_URL "https://id.twitch.tv/oauth2/token"
#define USER_URL "https://api.twitch.tv/helix/users"
#else
#define SITE "Kick"
#define SCOPES "chat:write user:read"
#define TOKEN_URL "https://id.kick.com/oauth/token"
#define USER_URL "https://api.kick.com/public/v1/users"
#endif
#ifdef AUTH_MOCK
#undef TOKEN_URL
#undef USER_URL
#define TOKEN_URL "http://127.0.0.1:8989/token"
#define USER_URL "http://127.0.0.1:8989/users"
#endif
#define TOKEN_MAX 2048
static int follow_scope;static char user_id[32];
static char cid[129],relay[384],token[TOKEN_MAX],refresh[TOKEN_MAX],name[64];
static char info[192]="Sin sesion",link[2048],ucode[80],device[512];
static time_t expires,checked,pending_until,next_poll,retry_after,last_send;static int storage_failed;static int ready,interval=5;
#ifdef AUTH_MOCK
int auth_test_stage;
#endif
/* Local flow age uses monotonic ticks, never RTC while this process runs. */
static uint64_t flow_started_ms, poll_at_ms;
static unsigned flow_lifetime;
static int flow_active;
static uint64_t flow_millis(void) {
#ifdef AUTH_HOST_TEST
    extern uint64_t auth_test_millis(void);
    return auth_test_millis();
#else
    return ticks_to_millisecs(gettime());
#endif
}
static void flow_arm(unsigned life){flow_started_ms=flow_millis();flow_lifetime=life;flow_active=1;}
static int flow_expired(void){uint64_t now=flow_millis();return flow_active&&now>=flow_started_ms&&now-flow_started_ms>=(uint64_t)flow_lifetime*1000;}
static void field(const char *json,const char *key,char *out,int sz){const char *p=json?jget(json,key):NULL;out[0]=0;if(p&&!jis_null(p))jstr(p,out,sz);}
static long numeric_field(const char*json,const char*key){const char*p=jget(json,key);if(!p)return 0;p=jws(p);if(*p!='"')return (long)jnum(p);char b[32],*end;field(json,key,b,sizeof(b));errno=0;long v=strtol(b,&end,10);return errno||end==b||*end?0:v;}
static int safe_value(const char *s,size_t max){size_t n=strlen(s);if(!n||n>=max)return 0;for(size_t i=0;i<n;i++)if((unsigned char)s[i]<33||(unsigned char)s[i]>126)return 0;return 1;}
static int identifier(const char*s){size_t n=strlen(s);if(n<4||n>128||strstr(s,"TODO")||strstr(s,"PLACEHOLDER"))return 0;for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='-'&&s[i]!='_')return 0;return 1;}
/* la sesion se guarda en el dispositivo montado (SD primero, si no USB).
 * Antes era siempre sd:/: sin SD (solo USB) el guardado fallaba y la cuenta
 * quedaba solo en RAM, asi que al salir y volver a entrar se perdia. */
#ifdef AUTH_HOST_TEST
static int cfg_dirs(char d[2][96],int create){(void)create;snprintf(d[0],96,"%s",TV_CFG_DIR);mkdir(TV_CFG_DIR,0777);return 1;}
#else
static int cfg_dirs(char d[2][96],int create){return fs_data_dirs(TV_APP_NAME,d,create);}
#endif
static int atom(const char *fn,const void *bytes,size_t n){
 char d[2][96];if(cfg_dirs(d,1)<1)return 0;
 char path[192],tmp[256],bak[256];snprintf(path,sizeof(path),"%s%s",d[0],fn);snprintf(tmp,sizeof(tmp),"%s.tmp",path);snprintf(bak,sizeof(bak),"%s.bak",path);
 FILE*f=fopen(tmp,"wb");if(!f)return 0;int ok=fwrite(bytes,1,n,f)==n;ok=fflush(f)==0&&ok;ok=fclose(f)==0&&ok;if(!ok){remove(tmp);return 0;}
 remove(bak);int had=rename(path,bak)==0;if(rename(tmp,path)){if(had)rename(bak,path);remove(tmp);return 0;}remove(bak);return 1;
}
static int readfile(const char *fn,char *buf,int cap){char d[2][96];int nd=cfg_dirs(d,0);for(int i=0;i<nd;i++)for(int b=0;b<2;b++){char path[256];snprintf(path,sizeof(path),"%s%s%s",d[i],fn,b?".bak":"");FILE*f=fopen(path,"rb");if(!f)continue;size_t n=fread(buf,1,cap-1,f);int extra=fgetc(f);fclose(f);buf[n]=0;if(extra==EOF&&n)return (int)n;}buf[0]=0;return 0;}
static void erasefile(const char*fn){char d[2][96];int nd=cfg_dirs(d,0);const char *suffix[]={"",".bak",".tmp"};for(int i=0;i<nd;i++)for(int k=0;k<3;k++){char p[256];snprintf(p,sizeof(p),"%s%s%s",d[i],fn,suffix[k]);remove(p);}}
static char saved_login[64];
static int relay_ok(const char*s){
 if(!s||strlen(s)<9||strlen(s)>350||strstr(s,"TODO")||strstr(s,"PLACEHOLDER")||strpbrk(s,"?#@ \r\n\t"))return 0;
#ifdef AUTH_MOCK
 if(!strcmp(s,"http://127.0.0.1:8789"))return 1;
#endif
 return !strncmp(s,"https://",8)&&s[8]&&strchr(s+8,'.')!=NULL;
}
static void config(void){
 cid[0]=relay[0]=0;
#ifdef WII_TWITCH_AUTH
 if(identifier(DEFAULT_TWITCH_CLIENT_ID))snprintf(cid,sizeof(cid),"%s",DEFAULT_TWITCH_CLIENT_ID);
#ifdef AUTH_MOCK
 snprintf(cid,sizeof(cid),"personal-fixture-app");
#endif
#else
 if(identifier(OAUTH_KICK_CLIENT_ID))snprintf(cid,sizeof(cid),"%s",OAUTH_KICK_CLIENT_ID);
 if(relay_ok(DEFAULT_KICK_RELAY_URL))snprintf(relay,sizeof(relay),"%s",DEFAULT_KICK_RELAY_URL);
#ifdef AUTH_MOCK
 snprintf(relay,sizeof(relay),"http://127.0.0.1:8789");snprintf(cid,sizeof(cid),"personal-fixture-app");
#endif
#endif
 size_t n=strlen(relay);while(n&&relay[n-1]=='/')relay[--n]=0;
}
int auth_configured(void){
#ifdef WII_TWITCH_AUTH
 return cid[0]!=0;
#else
 return relay[0]!=0&&cid[0]!=0;
#endif
}
const char *auth_client_id(void){return cid;}
static const char*kick_endpoint(const char*part){static char out[448];snprintf(out,sizeof(out),"%s/%s",relay,part);return out;}
#ifdef SEND_DRYRUN
int auth_ready(void){return 1;}   /* sin cuenta real; nada se envia */
#else
int auth_ready(void){return ready;}
#endif
const char *auth_access_token(void){return ready&&token[0]?token:"";}
int auth_has_session(void){return token[0]!=0;}
const char *auth_login(void){return name;}
const char *auth_status(void){return T(info);}
const char *auth_url(void){return link;}
const char *auth_code(void){return ucode;}
typedef struct{char data[16384];size_t n;} reply;
static size_t write_reply(char*p,size_t s,size_t n,void*u){reply*r=u;size_t k=s*n;if(k>=sizeof(r->data)-r->n)return 0;memcpy(r->data+r->n,p,k);r->n+=k;r->data[r->n]=0;return k;}
static long request(const char *url,const char *body,int bearer,reply*r){
 memset(r,0,sizeof(*r));if(!sh_net_ready())return 0;CURL*c=curl_easy_init();if(!c)return 0;char err[CURL_ERROR_SIZE],authorization[TOKEN_MAX+32],client[160];sh_curl_common(c,err);
 curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
#ifdef AUTH_MOCK
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"HTTP");
#else
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"HTTPS");
#endif
 curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(c,CURLOPT_TIMEOUT,10L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
 curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_reply);curl_easy_setopt(c,CURLOPT_WRITEDATA,r);
 struct curl_slist*h=NULL;h=curl_slist_append(h,"Accept: application/json");
 if(body){curl_easy_setopt(c,CURLOPT_POSTFIELDS,body);h=curl_slist_append(h,"Content-Type: application/x-www-form-urlencoded");}
 if(bearer){snprintf(authorization,sizeof(authorization),"Authorization: %s %s",bearer==2?"OAuth":"Bearer",token);h=curl_slist_append(h,authorization);
#ifdef WII_TWITCH_AUTH
 snprintf(client,sizeof(client),"Client-Id: %s",cid);h=curl_slist_append(h,client);
#endif
}
 curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);CURLcode rc=curl_easy_perform(c);long code=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);curl_slist_free_all(h);curl_easy_cleanup(c);memset(authorization,0,sizeof(authorization));return rc==CURLE_OK?code:0;
}
/* Form builder refuses truncation, and percent-encodes every value. */
static int pair(char *b,size_t cap,const char*k,const char*v){char*e=curl_easy_escape(NULL,v,0);if(!e)return 0;size_t need=strlen(b)+strlen(k)+strlen(e)+3;if(need>cap){curl_free(e);return 0;}strcat(b,b[0]?"&":"");strcat(b,k);strcat(b,"=");strcat(b,e);curl_free(e);return 1;}
static int has_scope(const char *json,const char*key,const char*want){
 const char*p=jget(json,key);if(!p)return 0;p=jws(p);
 if(*p=='['){for(const char*e=jarr_first(p);e;e=jarr_next(e)){char scope[128];jstr(e,scope,sizeof(scope));if(!strcmp(scope,want))return 1;}return 0;}
 char buf[512];jstr(p,buf,sizeof(buf));char*save,*word=strtok_r(buf," ",&save);while(word){if(!strcmp(word,want))return 1;word=strtok_r(NULL," ",&save);}return 0;
}
static int scopes_ok(const char*json,const char*key){
#ifdef WII_TWITCH_AUTH
 return has_scope(json,key,"chat:read")&&has_scope(json,key,"chat:edit");
#else
 return has_scope(json,key,"chat:write")&&has_scope(json,key,"user:read");
#endif
}
static int persist_tokens(void){size_t cap=1024+2*TOKEN_MAX;char*b=malloc(cap);if(!b)return 0;int n=snprintf(b,cap,"client_id=%s\nissuer=%s\naccess_token=%s\nrefresh_token=%s\nexpires=%lld\nlogin=%s\nuser_id=%s\n",cid,relay,token,refresh,(long long)expires,name,user_id);int ok=n>0&&(size_t)n<cap&&atom("oauth-tokens.txt",b,n);if(ok)snprintf(saved_login,sizeof(saved_login),"%s",name);memset(b,0,cap);free(b);return ok;}
void auth_logout(void){follow_scope=0;user_id[0]=0;flow_active=0;poll_at_ms=0;ready=0;memset(token,0,sizeof(token));memset(refresh,0,sizeof(refresh));memset(device,0,sizeof(device));name[0]=link[0]=ucode[0]=0;expires=checked=pending_until=next_poll=retry_after=last_send=0;saved_login[0]=0;erasefile("oauth-tokens.txt");erasefile("oauth-pending.txt");erasefile("oauth-callback.txt");erasefile("oauth-url.txt");snprintf(info,sizeof(info),T("Sesion cerrada en esta SD"));}
static int tokens(const char *json,int is_refresh){
 char nt[TOKEN_MAX],nr[TOKEN_MAX];field(json,"access_token",nt,sizeof(nt));field(json,"refresh_token",nr,sizeof(nr));long life=numeric_field(json,"expires_in");
 /* al renovar, si el proveedor no repite refresh_token/scope se conservan los anteriores. */
 if(is_refresh&&!nr[0]&&refresh[0])snprintf(nr,sizeof(nr),"%s",refresh);int keep_scope=is_refresh&&!jget(json,"scope");
 if(!safe_value(nt,sizeof(nt))||!safe_value(nr,sizeof(nr))||life<30||life>31536000){snprintf(info,sizeof(info),T("Respuesta OAuth incompleta; inicia sesion otra vez"));return 0;}
 if(!keep_scope&&!scopes_ok(json,"scope")){snprintf(info,sizeof(info),T("Faltan permisos de chat; vuelve a autorizar"));return 0;}
 snprintf(token,sizeof(token),"%s",nt);snprintf(refresh,sizeof(refresh),"%s",nr);expires=sh_clock_now()+life;ready=0;checked=retry_after=0;
 memset(nt,0,sizeof(nt));memset(nr,0,sizeof(nr));int saved=persist_tokens();storage_failed=!saved;snprintf(info,sizeof(info),saved?T("Token recibido; verificando cuenta..."):T("Token recibido. ERROR al guardar SD: sesion solo temporal"));return 1;
}
static int renew(void){char b[8192]="";reply*r=calloc(1,sizeof(*r));if(!r)return 0;if(!refresh[0]){free(r);return 0;}
 int ok=pair(b,sizeof(b),"refresh_token",refresh);
#ifdef WII_TWITCH_AUTH
 ok=ok&&pair(b,sizeof(b),"client_id",cid)&&pair(b,sizeof(b),"grant_type","refresh_token");const char*url=TOKEN_URL;
#else
 const char*url=kick_endpoint("refresh");
#endif
 long c=ok?request(url,b,0,r):0;memset(b,0,sizeof(b));ok=c==200&&tokens(r->data,1);
 if(c==400||c==401){auth_logout();snprintf(info,sizeof(info),T("Sesion vencida/revocada: autoriza otra vez"));}else if(!ok)snprintf(info,sizeof(info),T("No se pudo renovar OAuth (HTTP %ld)"),c);memset(r,0,sizeof(*r));free(r);return ok;
}
int auth_refresh_now(void){if(!auth_has_session())return 0;ready=0;checked=retry_after=0;return renew()&&auth_ensure();}
int auth_due(void){return token[0]&&sh_clock_now()>=retry_after&&(!checked||sh_clock_now()-checked>=3500||sh_clock_now()>=expires-90);}
int auth_ensure(void){
 if(!token[0])return 0;if(!auth_due()&&ready)return 1;if(sh_clock_now()<retry_after)return 0;retry_after=sh_clock_now()+60;ready=0;
 /* si renew falla por red la sesion queda guardada y se reintenta pronto; solo 400/401 del proveedor la borran. */
 if(sh_clock_now()>=expires-90&&!renew()){if(token[0])retry_after=sh_clock_now()+15;return 0;}retry_after=sh_clock_now()+60;
 int renewed=0;
 reply*r=calloc(1,sizeof(*r));if(!r)return 0;long c;
again:
#ifdef WII_TWITCH_AUTH
 #ifdef AUTH_MOCK
 const char *validate="http://127.0.0.1:8989/validate";
 #else
 const char *validate="https://id.twitch.tv/oauth2/validate";
 #endif
 c=request(validate,NULL,2,r);char gotid[129];field(r->data,"client_id",gotid,sizeof(gotid));if(c==401){/* el reloj de la Wii (hora local) puede hacer creer que el token sigue vigente: renovar antes de borrar. */if(!renewed&&refresh[0]){renewed=1;if(renew())goto again;free(r);if(token[0])retry_after=sh_clock_now()+15;return 0;}auth_logout();snprintf(info,sizeof(info),T("Twitch revoco la sesion. Vuelve a autorizar."));free(r);return 0;}
 if(c!=200||strcmp(gotid,cid)||!scopes_ok(r->data,"scopes")){snprintf(info,sizeof(info),T("No se pudo validar cliente/permisos Twitch (HTTP %ld)"),c);free(r);return 0;}
 follow_scope=has_scope(r->data,"scopes","user:read:follows");
 long remaining=(long)jnum(jget(r->data,"expires_in"));if(remaining<=0){free(r);return 0;}expires=sh_clock_now()+remaining;
#endif
 c=request(USER_URL,NULL,1,r);if(c==401){if(!renewed&&refresh[0]){renewed=1;if(renew())goto again;free(r);if(token[0])retry_after=sh_clock_now()+15;return 0;}auth_logout();snprintf(info,sizeof(info),T("Token rechazado; inicia sesion otra vez"));free(r);return 0;}
 const char*arr=jget(r->data,"data"),*u=arr?jarr_first(arr):NULL;
#ifdef WII_TWITCH_AUTH
 field(u,"login",name,sizeof(name));field(u,"id",user_id,sizeof(user_id));for(const char*p=user_id;*p;p++)if(!isdigit((unsigned char)*p)){user_id[0]=0;break;}
#else
 field(u,"name",name,sizeof(name));
#endif
 if(c==0){if(saved_login[0])snprintf(name,sizeof(name),"%s",saved_login);retry_after=sh_clock_now()+15;snprintf(info,sizeof(info),T("Sesion guardada; esperando red para verificar"));free(r);return 0;}
 if(c!=200||!auth_message_valid(name)){name[0]=0;if(saved_login[0])snprintf(name,sizeof(name),"%s",saved_login);snprintf(info,sizeof(info),T("No se pudo identificar la cuenta (HTTP %ld)"),c);free(r);return 0;}
 ready=1;retry_after=0;checked=sh_clock_now();if(strcmp(saved_login,name)&&!persist_tokens())storage_failed=1;snprintf(info,sizeof(info),storage_failed?T("Cuenta %s: ERROR SD, tokens solo en RAM"):T("Sesion: %s"),name);free(r);return 1;
}
int auth_begin(void){config();if(!auth_configured()){snprintf(info,sizeof(info),T("Version sin configurar por el desarrollador"));return 0;}auth_logout();char b[1024]="";reply*r=calloc(1,sizeof(*r));if(!r)return 0;
#ifdef WII_TWITCH_AUTH
 pair(b,sizeof(b),"client_id",cid);pair(b,sizeof(b),"scopes",SCOPES);
#ifdef AUTH_MOCK
 const char*url="http://127.0.0.1:8989/device";
#else
 const char*url="https://id.twitch.tv/oauth2/device";
#endif
#else
 pair(b,sizeof(b),"client_id",cid);const char*url=kick_endpoint("device");
#endif
 long c=request(url,b,0,r);field(r->data,"device_code",device,sizeof(device));field(r->data,"user_code",ucode,sizeof(ucode));
#ifdef WII_TWITCH_AUTH
 field(r->data,"verification_uri",link,sizeof(link));int trusted=!strcmp(link,"https://www.twitch.tv/activate")||!strncmp(link,"https://www.twitch.tv/activate?",sizeof("https://www.twitch.tv/activate?")-1);
#else
 field(r->data,"verification_url",link,sizeof(link));char prefix[448];snprintf(prefix,sizeof(prefix),"%s/k/",relay);int trusted=!strncmp(link,prefix,strlen(prefix));
#endif
 long life=(long)jnum(jget(r->data,"expires_in"));interval=(int)jnum(jget(r->data,"interval"));if(interval<5)interval=5;if(interval>120)interval=120;
 int ok=c==200&&safe_value(device,sizeof(device))&&safe_value(ucode,sizeof(ucode))&&trusted&&life>0&&life<=3600;
#ifdef WII_TWITCH_AUTH
 if(ok){char*e=curl_easy_escape(NULL,ucode,0);if(e){snprintf(link,sizeof(link),"https://www.twitch.tv/activate?public=true&device-code=%s",e);curl_free(e);}else ok=0;}
#endif
 memset(r,0,sizeof(*r));free(r);if(!ok){device[0]=ucode[0]=link[0]=0;snprintf(info,sizeof(info),T("No se pudo obtener QR (HTTP %ld). Revisa red/configuracion del desarrollador"),c);return 0;}
 flow_arm((unsigned)life);poll_at_ms=flow_millis()+(unsigned)interval*1000;snprintf(info,sizeof(info),T("Escanea y autoriza con TU cuenta; esperando..."));return 1;
}
int auth_poll(void){if(!device[0])return 0;if(flow_expired()){device[0]=link[0]=ucode[0]=0;flow_active=0;snprintf(info,sizeof(info),T("Codigo vencido: pulsa Nuevo QR"));return -1;}if(flow_millis()<poll_at_ms)return 0;poll_at_ms=flow_millis()+(unsigned)interval*1000;
 char b[2048]="";pair(b,sizeof(b),"device_code",device);
#ifdef WII_TWITCH_AUTH
 pair(b,sizeof(b),"client_id",cid);pair(b,sizeof(b),"grant_type","urn:ietf:params:oauth:grant-type:device_code");pair(b,sizeof(b),"scopes",SCOPES);const char*url=TOKEN_URL;
#else
 const char*url=kick_endpoint("token");
#endif
 reply*r=calloc(1,sizeof(*r));if(!r)return -1;long c=request(url,b,0,r);memset(b,0,sizeof(b));char msg[128],error[128];field(r->data,"message",msg,sizeof(msg));field(r->data,"error",error,sizeof(error));
 if(c==200){int ok=tokens(r->data,0);device[0]=link[0]=ucode[0]=0;flow_active=0;memset(r,0,sizeof(*r));free(r);return ok&&auth_ensure()?1:-1;}
 if(!strcmp(msg,"authorization_pending")||!strcmp(error,"authorization_pending")){free(r);return 0;}
 if(!strcmp(msg,"slow_down")||!strcmp(error,"slow_down")){interval+=5;poll_at_ms=flow_millis()+(unsigned)interval*1000;free(r);return 0;}
 if(c==0||c>=500||c==429){poll_at_ms=flow_millis()+15000;snprintf(info,sizeof(info),T("Esperando red/relay; no se reenvia ningun mensaje"));free(r);return 0;}
 device[0]=link[0]=ucode[0]=0;flow_active=0;snprintf(info,sizeof(info),T("Autorizacion denegada/vencida (HTTP %ld). Nuevo QR."),c);free(r);return -1;
}
int auth_import_callback(const char*u){(void)u;snprintf(info,sizeof(info),T("Esta version usa QR publico; no importa PKCE/codigos manuales"));return 0;}
int auth_import_file(void){return auth_import_callback(NULL);}
void auth_init(void){saved_login[0]=0;follow_scope=0;user_id[0]=0;flow_active=0;poll_at_ms=0;ready=0;token[0]=refresh[0]=name[0]=device[0]=link[0]=ucode[0]=0;expires=checked=pending_until=next_poll=retry_after=last_send=0;storage_failed=0;config();
 /* PKCE state is not valid for public relay login. Do not read old RTCs. */
 erasefile("oauth-pending.txt");erasefile("oauth-callback.txt");erasefile("oauth-url.txt");char b[1024+2*TOKEN_MAX];
 if(readfile("oauth-tokens.txt",b,sizeof(b))){char stored[129]="",issuer[384]="",*save,*ln=strtok_r(b,"\r\n",&save);while(ln){char*eq=strchr(ln,'=');if(eq){*eq++=0;if(!strcmp(ln,"client_id"))snprintf(stored,sizeof(stored),"%s",eq);if(!strcmp(ln,"issuer"))snprintf(issuer,sizeof(issuer),"%s",eq);if(!strcmp(ln,"access_token")&&safe_value(eq,sizeof(token)))snprintf(token,sizeof(token),"%s",eq);if(!strcmp(ln,"refresh_token")&&safe_value(eq,sizeof(refresh)))snprintf(refresh,sizeof(refresh),"%s",eq);if(!strcmp(ln,"expires"))expires=(time_t)strtoll(eq,NULL,10);if(!strcmp(ln,"login")&&strlen(eq)<sizeof(name)&&auth_message_valid(eq))snprintf(name,sizeof(name),"%s",eq);if(!strcmp(ln,"user_id")&&strlen(eq)<sizeof(user_id)&&strspn(eq,"0123456789")==strlen(eq))snprintf(user_id,sizeof(user_id),"%s",eq);}ln=strtok_r(NULL,"\r\n",&save);}
#ifdef WII_TWITCH_AUTH
 int match=auth_configured()&&!strcmp(cid,stored);
#else
 int match=auth_configured()&&!strcmp(relay,issuer)&&!strcmp(cid,stored);
#endif
 if(!match){token[0]=refresh[0]=0;expires=0;name[0]=user_id[0]=0;}if(!token[0])name[0]=0;snprintf(saved_login,sizeof(saved_login),"%s",name);memset(b,0,sizeof(b));}
 snprintf(info,sizeof(info),!auth_configured()?T("Version sin configurar por el desarrollador"):token[0]?T("Sesion guardada; verificando en red..."):T("Abri Iniciar sesion: escanea el QR y autoriza"));
}
int auth_irc_snapshot(char *out,int ts,char *who,int ns){if(!ready||!token[0]||!name[0]||strlen(token)>=(size_t)ts)return 0;for(const char*p=name;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='_')return 0;snprintf(out,ts,"%s",token);snprintf(who,ns,"%s",name);return 1;}
/* Conservative 350-byte limit also keeps a complete IRC line under 512 bytes. */
int auth_message_valid(const char *text){if(!text)return 0;size_t n=strlen(text);if(!n||n>350)return 0;int nonspace=0;for(size_t i=0;i<n;){unsigned char c=text[i];if(c<32||c==127)return 0;if(c!=' ')nonspace=1;if(c<128){i++;continue;}int k=c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;if(!k||i+k>n)return 0;unsigned cp=c&((1<<(7-k))-1);for(int j=1;j<k;j++){unsigned char d=text[i+j];if((d&0xc0)!=0x80)return 0;cp=(cp<<6)|(d&63);}if((k==3&&cp<0x800)||(k==4&&cp<0x10000)||(cp>=0xd800&&cp<=0xdfff)||cp>0x10ffff)return 0;i+=k;}return nonspace;}
int auth_kick_send(unsigned long broadcaster,const char *text){
#ifdef WII_TWITCH_AUTH
 (void)broadcaster;(void)text;return 0;
#else
 if(!ready||!broadcaster||!auth_message_valid(text))return 0;if(last_send&&sh_clock_now()-last_send<2){snprintf(info,sizeof(info),T("Espera dos segundos entre mensajes"));return 0;}last_send=sh_clock_now();char body[2300],escaped[2200];size_t j=0;for(size_t i=0;text[i];i++){if(text[i]=='"'||text[i]=='\\')escaped[j++]='\\';escaped[j++]=text[i];}escaped[j]=0;snprintf(body,sizeof(body),"{\"broadcaster_user_id\":%lu,\"content\":\"%s\",\"type\":\"user\"}",broadcaster,escaped);
 #ifdef AUTH_MOCK
 const char*url="http://127.0.0.1:8989/chat";
 #else
 const char*url="https://api.kick.com/public/v1/chat";
 #endif
 reply*r=calloc(1,sizeof(*r));if(!r)return 0;CURL*c=curl_easy_init();if(!c){free(r);return 0;}char eb[CURL_ERROR_SIZE],h[TOKEN_MAX+32];sh_curl_common(c,eb);curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(c,CURLOPT_TIMEOUT,10L);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(c,CURLOPT_POSTFIELDS,body);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_reply);curl_easy_setopt(c,CURLOPT_WRITEDATA,r);snprintf(h,sizeof(h),"Authorization: Bearer %s",token);struct curl_slist*headers=NULL;headers=curl_slist_append(headers,h);headers=curl_slist_append(headers,"Content-Type: application/json");curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);CURLcode rc=curl_easy_perform(c);long code=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);curl_slist_free_all(headers);curl_easy_cleanup(c);memset(h,0,sizeof(h));
 const char*sent=jpath(r->data,"data.is_sent");int ok=rc==CURLE_OK&&code==200&&sent&&!strncmp(jws(sent),"true",4);if(code==401){if(auth_refresh_now())snprintf(info,sizeof(info),T("Sesion renovada: vuelve a enviar (no se reenvio)."));else snprintf(info,sizeof(info),T("Sesion rechazada: autoriza de nuevo. No se reenvio."));}else if(ok)snprintf(info,sizeof(info),T("Mensaje aceptado por Twitch"));else snprintf(info,sizeof(info),T("Envio no confirmado (HTTP %ld). Sin reintento automatico."),code);free(r);return ok;
#endif
}

#ifdef AUTH_MOCK
int auth_mock_prepare(void){return 0;}
int auth_mock_complete(void){
#ifdef WII_TWITCH_AUTH
 return 0;
#else
 char b[2300]="";pair(b,sizeof(b),"url",link);reply*r=calloc(1,sizeof(*r));if(!r)return 0;long c=request("http://127.0.0.1:8789/__test/approve",b,0,r);free(r);return c==200;
#endif
}
#endif
#ifdef WII_TWITCH_AUTH
int auth_follows_allowed(void){return ready&&follow_scope&&user_id[0];}
const char *auth_user_id(void){return user_id;}
/* GET a Helix con la sesion actual (emotes del chat: no piden permisos extra). */
long auth_helix_get(const char *pathq,sh_resp_t *r){
 memset(r,0,sizeof(*r));if(!pathq||strlen(pathq)>400)return 0;
 if(auth_due())auth_ensure();
 if(!ready||!token[0])return 0;
 char url[480],a[TOKEN_MAX+32],c[160];
#ifdef AUTH_MOCK
 snprintf(url,sizeof(url),"http://127.0.0.1:8989/helix/%s",pathq);
#else
 snprintf(url,sizeof(url),"https://api.twitch.tv/helix/%s",pathq);
#endif
 for(int t=0;t<2;t++){
  snprintf(a,sizeof(a),"Authorization: Bearer %s",token);snprintf(c,sizeof(c),"Client-Id: %s",cid);
  const char*h[]={a,c,"Accept: application/json",NULL};
  sh_request("GET",url,h,NULL,NULL,r);memset(a,0,sizeof(a));
  if(r->code!=401||t||!auth_refresh_now())break;
  sh_resp_free(r);memset(r,0,sizeof(*r));
 }
 return r->code;
}
long auth_follow_read(int endpoint,const char *query,char *out,size_t cap){
 if(!out||!cap)return 0;out[0]=0;if(auth_due()&&!auth_ensure())return 0;if(!auth_ready())return 401;if(!auth_follows_allowed())return 403;
 static const char *paths[]={"users","channels/followed","streams/followed"};if(endpoint<0||endpoint>2||!query||strlen(query)>2048)return 0;
 char url[2300];
#ifdef AUTH_MOCK
 snprintf(url,sizeof(url),"http://127.0.0.1:8989/helix/%s?%s",paths[endpoint],query);
#else
 snprintf(url,sizeof(url),"https://api.twitch.tv/helix/%s?%s",paths[endpoint],query);
#endif
 reply*r=calloc(1,sizeof(*r));if(!r)return 0;long code=request(url,NULL,1,r);
 if(code==401&&auth_refresh_now())code=request(url,NULL,1,r);
 if(code==200&&r->n<cap)memcpy(out,r->data,r->n+1);else if(code==200)code=0;
memset(r,0,sizeof(*r));free(r);return code;
}

#endif
