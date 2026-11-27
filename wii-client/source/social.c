#define SOCIAL_TWITCH
/* Official follow reads + phone handoff. No follow mutation or payment API. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <curl/curl.h>
#ifdef SOCIAL_HOST_TEST
#include "auth_test_shim.h"
#include "auth.h"
#else
#include <gccore.h>
#include <wiiuse/wpad.h>
#include "tv.h"
#endif
#include "social.h"
#include "qrcodegen.h"
#define OX ((SCR_W-640)/2)
#ifdef SOCIAL_MOCK
int social_test_stage;
#endif
static char status_text[192]="Estado sin consultar";
/* Slugs, never arbitrary URLs: QR destinations stay on the official domain. */
static int slug_ok(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (!n || n > 25) return 0;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_'
#ifndef SOCIAL_TWITCH
              || *s == '-'
#endif
              )) return 0;
    }
    return 1;
}
static int canonical(const char*in,char*out,size_t cap){if(!slug_ok(in)||strlen(in)>=cap)return 0;for(size_t i=0;;i++){out[i]=tolower((unsigned char)in[i]);if(!in[i])break;}return 1;}
int social_url(const char*slug,int sub,char*out,size_t cap){char name[48];if(!canonical(slug,name,sizeof(name))||!out||!cap)return 0;
#ifdef SOCIAL_TWITCH
 int n=snprintf(out,cap,sub?"https://www.twitch.tv/subs/%s":"https://www.twitch.tv/%s",name);
#else
 (void)sub;int n=snprintf(out,cap,"https://kick.com/%s",name);
#endif
 return n>0&&(size_t)n<cap;
}
#ifndef SOCIAL_TWITCH
static char favorites[64][48];static int nfavs,loaded;
static int find_fav(const char*slug){for(int i=0;i<nfavs;i++)if(!strcmp(favorites[i],slug))return i;return -1;}
static void favorites_load(void){if(loaded)return;loaded=1;nfavs=0;FILE*f=fopen(TV_CFG_DIR "favoritos.txt","rb");if(!f)f=fopen(TV_CFG_DIR "favoritos.txt.bak","rb");if(!f)return;char line[256],name[48];while(fgets(line,sizeof(line),f)){if(!strchr(line,'\n')&&!feof(f)){int c;while((c=fgetc(f))!=EOF&&c!='\n');continue;}line[strcspn(line,"\r\n")]=0;if(nfavs<64&&canonical(line,name,sizeof(name))&&find_fav(name)<0)snprintf(favorites[nfavs++],48,"%s",name);}fclose(f);}
int social_favorite(const char*slug){char n[48];if(!canonical(slug,n,sizeof(n)))return 0;favorites_load();return find_fav(n)>=0;}
int social_toggle_favorite(const char*slug){char n[48];if(!canonical(slug,n,sizeof(n)))return 0;favorites_load();char backup[64][48];memcpy(backup,favorites,sizeof(backup));int old=nfavs,idx=find_fav(n);if(idx>=0){if(idx<nfavs-1)memmove(favorites[idx],favorites[idx+1],(nfavs-idx-1)*48);nfavs--;}else{if(nfavs==64){snprintf(status_text,sizeof(status_text),T("Limite: 64 favoritos locales"));return 0;}snprintf(favorites[nfavs++],48,"%s",n);}
 FILE*f=fopen(TV_CFG_DIR "favoritos.txt.tmp","wb");int ok=f!=NULL;if(f){for(int i=0;i<nfavs;i++)if(fprintf(f,"%s\n",favorites[i])<0)ok=0;if(fclose(f))ok=0;}
 if(ok){remove(TV_CFG_DIR "favoritos.txt.bak");int had=rename(TV_CFG_DIR "favoritos.txt",TV_CFG_DIR "favoritos.txt.bak")==0;if(rename(TV_CFG_DIR "favoritos.txt.tmp",TV_CFG_DIR "favoritos.txt")){if(had)rename(TV_CFG_DIR "favoritos.txt.bak",TV_CFG_DIR "favoritos.txt");ok=0;}else remove(TV_CFG_DIR "favoritos.txt.bak");}
 if(!ok){nfavs=old;memcpy(favorites,backup,sizeof(backup));remove(TV_CFG_DIR "favoritos.txt.tmp");snprintf(status_text,sizeof(status_text),T("No se guardo el favorito: revisa la SD"));return 0;}
 snprintf(status_text,sizeof(status_text),T("Favorito SD actualizado. NO cambia tu cuenta Twitch."));return 1;
}
#else
static char cached_slug[48],cached_login[64];static int followed=-1;static time_t last_query;
static int digits(const char*s){if(!s||!*s)return 0;for(;*s;s++)if(!isdigit((unsigned char)*s))return 0;return 1;}
int social_refresh(const char*slug){char n[48];if(!canonical(slug,n,sizeof(n)))return -1;if(strcmp(n,cached_slug)||strcmp(cached_login,auth_login())){last_query=0;followed=-1;}snprintf(cached_slug,sizeof(cached_slug),"%s",n);snprintf(cached_login,sizeof(cached_login),"%s",auth_login());
 if(!auth_follows_allowed()){followed=-1;snprintf(status_text,sizeof(status_text),T("Para ver el estado: inicia sesion con user:read:follows"));return -1;}
 time_t now=time(NULL);if(last_query&&now>=last_query&&now-last_query<5)return followed;last_query=now;followed=-1;
 char *json=calloc(1,16384);if(!json)return -1;char q[512],id[32];snprintf(q,sizeof(q),"login=%s",n);long code=auth_follow_read(0,q,json,16384);const char*data=jget(json,"data"),*u=data?jarr_first(data):NULL;id[0]=0;if(u){const char*v=jget(u,"id");if(v)jstr(v,id,sizeof(id));}if(code==200&&!digits(id))code=404;
 if(code==200){snprintf(q,sizeof(q),"user_id=%s&broadcaster_id=%s&first=1",auth_user_id(),id);code=auth_follow_read(1,q,json,16384);data=jget(json,"data");if(code==200&&data&&*jws(data)=='['){u=jarr_first(data);if(!u)followed=0;else{char got[32]="";const char*v=jget(u,"broadcaster_id");if(v)jstr(v,got,sizeof(got));if(!strcmp(got,id))followed=1;}}}
 if(followed>=0)snprintf(status_text,sizeof(status_text),followed?T("Siguiendo: confirmado por Helix (solo lectura)"):T("No lo seguis: confirmado por Helix (solo lectura)"));else snprintf(status_text,sizeof(status_text),T("Estado desconocido (HTTP %ld). No se cambia ningun follow."),code);free(json);return followed;
}
#endif
const char*social_label(const char*slug){
#ifdef SOCIAL_TWITCH
 char n[48];if(canonical(slug,n,sizeof(n))&&auth_ready()&&!strcmp(cached_login,auth_login())&&!strcmp(cached_slug,n)&&followed==1)return T("Siguiendo");return T("Seguir (web)");
#else
 return social_favorite(slug)?T("Favorito SD"):T("Seguir (web)");
#endif
}
#ifndef SOCIAL_HOST_TEST
static void qr(const char*url){uint8_t q[qrcodegen_BUFFER_LEN_FOR_VERSION(12)],temp[sizeof(q)];if(!qrcodegen_encodeText(url,temp,q,qrcodegen_Ecc_LOW,1,12,qrcodegen_Mask_AUTO,true))return;int n=qrcodegen_getSize(q),s=210/(n+8),x=46+OX+(210-(n+8)*s)/2,y=112;gfx_rect(x,y,(n+8)*s,(n+8)*s,0xffffff);for(int j=0;j<n;j++)for(int i=0;i<n;i++)if(qrcodegen_getModule(q,i,j))gfx_rect(x+(i+4)*s,y+(j+4)*s,s,s,0);}
static void wrap(int x,int y,const char*t,int w,u32 color){char**ln;int n=ui_wrap(&font_body,t,w,&ln);for(int i=0;i<n;i++){gfx_text(&font_body,x,y+i*20,ln[i],color);free(ln[i]);}free(ln);}
void social_screen(const char*slug,int sub){char url[160];if(!social_url(slug,sub,url,sizeof(url))){ui_toast(T("Canal invalido: no se abre ninguna URL"));return;}thumbs_pause(1);ui_set_backdrop(NULL);
#ifdef SOCIAL_MOCK
 social_test_stage=sub?3:2;
#endif
#ifdef SOCIAL_TWITCH
 if(!sub)social_refresh(slug);
#endif
 while(SYS_MainLoop()){u32 d=in_poll();ui_begin(d);if(d&(WPAD_BUTTON_HOME|WPAD_BUTTON_B))break;gfx_clear(C_BG);ui_panel(20+OX,20,600,440,sub?T("Suscribirse - telefono"):T("Seguir / dejar de seguir - telefono"));
#ifdef SOCIAL_MOCK
 gfx_text_c(&font_bold,SCR_W/2,72,T("SIMULACION LOCAL - NO SE REALIZA NINGUNA ACCION"),C_WARN);
#else
 gfx_text_c(&font_small,SCR_W/2,72,T("La Wii no sigue, no compra y no regala suscripciones."),C_DIM);
#endif
 qr(url);gfx_text_fit(&font_title,282+OX,110,slug,C_ACCENT_HI,310);wrap(282+OX,150,sub?T("Escanea y elegi Suscribirse en la web oficial. El pago se confirma SOLO en el telefono."):T("Escanea y toca Seguir o Dejar de seguir en la web oficial con TU cuenta."),300,C_TEXT);
#ifdef SOCIAL_TWITCH
 wrap(282+OX,252,sub?T("No se comprueba ni se confirma una compra desde la Wii."):T("Volver / Actualizar consulta el estado real. Si falta permiso, autoriza otra vez en Ajustes."),300,C_DIM);
 if(!sub&&ui_button(42+OX,366,270,30,T("Actualizar estado"),IC_NEXT,BS_NORMAL,0))social_refresh(slug);
#else
 wrap(282+OX,252,T("Twitch no publica tu estado de seguimiento. Favorito SD es solo un marcador local."),300,C_DIM);
 if(!sub&&ui_button(42+OX,366,270,30,social_favorite(slug)?T("Quitar favorito SD"):T("Guardar favorito SD"),IC_HEART,BS_ACCENT,0))social_toggle_favorite(slug);
#endif
 gfx_text_fit(&font_small,38+OX,338,url,C_LINK,564);gfx_text_fit(&font_small,38+OX,405,sub?T("No hay cobros desde esta app."):T(status_text),C_DIM,560);int back=ui_button(430+OX,420,168,30,T("Volver"),IC_BACK,BS_NORMAL,0);ui_present();if(back)break;}
#ifdef SOCIAL_TWITCH
 if(!sub)social_refresh(slug);
#endif
}
int social_home(char*out,size_t cap){if(!out||!cap)return 0;out[0]=0;thumbs_pause(1);ui_set_backdrop(NULL);
#ifdef SOCIAL_MOCK
 social_test_stage=4;
#endif
#ifdef SOCIAL_TWITCH
 if(!auth_follows_allowed()){sui_text_view(T("Seguidos"), T("B: volver"),T("Hace falta iniciar sesion otra vez para dar el nuevo permiso user:read:follows. Ajustes > Iniciar sesion > Nuevo codigo."));return 0;}
 char cursor[512]="";for(;;){char *json=calloc(1,16384);if(!json)return 0;char q[2300];char*enc=curl_easy_escape(NULL,cursor,0);if(!enc){free(json);return 0;}snprintf(q,sizeof(q),"user_id=%s&first=10%s%s",auth_user_id(),cursor[0]?"&after=":"",enc);curl_free(enc);long code=auth_follow_read(2,q,json,16384);const char*data=jget(json,"data");if(code!=200||!data||*jws(data)!='['){free(json);char msg[180];snprintf(msg,sizeof(msg),T("No se pudo consultar Seguidos (HTTP %ld). Estado desconocido; no se infiere una lista vacia."),code);sui_text_view("Seguidos", "B: volver",msg);return 0;}
 char labels[12][180],slugs[10][48],next[512]="";char*items[12];int n=1,count=0;snprintf(labels[0],180,T("Actualizar / primera pagina"));for(const char*e=jarr_first(data);e&&count<10;e=jarr_next(e)){char login[48]="",title[128]="",type[20]="";const char*v=jget(e,"user_login");if(v)jstr(v,login,sizeof(login));v=jget(e,"type");if(v)jstr(v,type,sizeof(type));if(!slug_ok(login)||strcmp(type,"live"))continue;v=jget(e,"title");if(v)jstr(v,title,sizeof(title));snprintf(slugs[count++],48,"%s",login);snprintf(labels[n++],180,"EN VIVO: %s\n%.120s",login,title);}
 const char*v=jpath(json,"pagination.cursor");if(v)jstr(v,next,sizeof(next));int more=next[0]&&strcmp(next,cursor);if(more)snprintf(labels[n++],180,T("Pagina siguiente"));free(json);for(int i=0;i<n;i++)items[i]=labels[i];int sel=0;
 u32 r=sui_list(T("Seguidos en vivo"),count?T("Helix oficial - A: abrir canal / B: volver"):T("Nadie en vivo en esta pagina - B: volver"),items,n,&sel);if(r!=WPAD_BUTTON_A)return 0;if(sel==0){cursor[0]=0;continue;}if(more&&sel==n-1){snprintf(cursor,sizeof(cursor),"%s",next);continue;}if(sel>0&&sel<=count){snprintf(out,cap,"%s",slugs[sel-1]);return 1;}}
#else
 favorites_load();if(!nfavs){sui_text_view(T("Favoritos SD"), T("B: volver"),T("Todavia no hay favoritos locales. Abri un canal > corazon Seguir > Guardar favorito SD. NO sigue al canal en tu cuenta Twitch."));return 0;}char*items[64];for(int i=0;i<nfavs;i++)items[i]=favorites[i];int sel=0;u32 r=sui_list(T("Favoritos SD (no son follows)"),T("A: abrir canal / B: volver - locales a esta SD"),items,nfavs,&sel);if(r==WPAD_BUTTON_A&&sel>=0&&sel<nfavs){snprintf(out,cap,"%s",favorites[sel]);return 1;}return 0;
#endif
}
#endif
