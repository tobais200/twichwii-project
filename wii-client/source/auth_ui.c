#define WII_TWITCH_AUTH
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include "tv.h"
#include "auth.h"
#include "qrcodegen.h"
#define OX ((SCR_W-640)/2)
static void wrap(int x,int y,const char*s,int w,u32 color){char**lines;int n=ui_wrap(&font_body,s,w,&lines);for(int i=0;i<n;i++){gfx_text(&font_body,x,y+i*20,lines[i],color);free(lines[i]);}free(lines);}
static void qr_draw(const char*url){uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(20)],tmp[sizeof(qr)];if(!url[0]||!qrcodegen_encodeText(url,tmp,qr,qrcodegen_Ecc_LOW,1,20,qrcodegen_Mask_AUTO,true))return;int n=qrcodegen_getSize(qr),scale=220/(n+8),x=42+OX+(220-(n+8)*scale)/2,y=100;gfx_rect(x,y,(n+8)*scale,(n+8)*scale,0xffffff);for(int j=0;j<n;j++)for(int i=0;i<n;i++)if(qrcodegen_getModule(qr,i,j))gfx_rect(x+(i+4)*scale,y+(j+4)*scale,scale,scale,0);}
void auth_screen(void){thumbs_pause(1);ui_set_backdrop(NULL);
 if(auth_due())auth_ensure();
 if(auth_configured()&&!auth_ready()&&!auth_has_session()&&!auth_url()[0]){gfx_clear(C_BG);ui_panel(20+OX,20,600,440,"Iniciar sesion");ui_spinner(120,200,24);gfx_text_c(&font_bold,SCR_W/2,260,"Preparando QR de autorizacion...",C_TEXT);ui_present();auth_begin();}
 while(SYS_MainLoop()){u32 d=in_poll();ui_begin(d);if(d&(WPAD_BUTTON_B|WPAD_BUTTON_HOME))break;auth_poll();
#ifdef AUTH_PUBLIC_MOCK
 auth_test_stage=auth_ready()?15:14;
#endif
 gfx_clear(C_BG);ui_panel(20+OX,20,600,440,"Iniciar sesion - escanea y autoriza");
#ifdef AUTH_MOCK
 gfx_text_c(&font_bold,SCR_W/2,72,"SIMULACION LOCAL - CUENTA Y SERVIDORES FALSOS",C_WARN);
#else
 gfx_text_c(&font_small,SCR_W/2,72,"No se piden Client IDs ni contrasenas de cuenta en la Wii.",C_DIM);
#endif
 if(!auth_configured()){
  gfx_icon(IC_USER,150+OX,180,64,C_ACCENT);gfx_text_fit(&font_title,270+OX,105,"Sin configurar",C_WARN,326);
  wrap(270+OX,150,"Version sin configurar por el desarrollador. El desarrollador debe completar la configuracion de login y recompilar. No tenes que crear una app ni ingresar IDs.",315,C_TEXT);
  wrap(44+OX,290,"Este paquete de prueba tiene las constantes de publicacion vacias. No se puede inventar un QR de autorizacion valido.",550,C_DIM);
 }else if(auth_ready()){
  gfx_icon(IC_USER,150+OX,180,64,C_ACCENT);gfx_text_c(&font_small,150+OX,240,"Cuenta conectada",C_DIM);gfx_text_fit(&font_title,282+OX,104,auth_login(),C_ACCENT,310);
  wrap(282+OX,150,"Listo. Volve al directo y pulsa Escribir. Corazon y Suscribirse abren la web oficial del canal en el telefono.",300,C_TEXT);
 }else if(auth_url()[0]){
  qr_draw(auth_url());gfx_text_fit(&font_title,282+OX,104,auth_code(),C_ACCENT,310);
#ifdef WII_TWITCH_AUTH
  wrap(282+OX,150,"Escanea el QR, inicia sesion en Twitch y autoriza TwitchWii. Tambien podes abrir twitch.tv/activate y escribir este codigo.",300,C_TEXT);
  wrap(282+OX,260,"Conexion directa a Twitch. Permisos: chat y lectura de Seguidos.",300,C_DIM);
#else
  wrap(282+OX,150,"Escanea, inicia sesion en Twitch y aproba. Al finalizar volve a la Wii: no copies URLs ni codigos.",300,C_TEXT);
  wrap(282+OX,260,"El relay del desarrollador solo gestiona OAuth. El chat va directo a Twitch.",300,C_DIM);
#endif
  gfx_text_fit(&font_small,42+OX,310,auth_url(),C_DIM,556);
 }else{gfx_icon(IC_USER,150+OX,180,64,C_ACCENT);wrap(282+OX,145,"No se obtuvo un QR. Revisa Internet y pulsa Nuevo QR. Si sigue fallando, contacta al desarrollador.",300,C_TEXT);}
 gfx_rrect(34+OX,332,572,30,8,C_PANEL2,255);gfx_text_fit(&font_small,44+OX,340,auth_status(),C_TEXT,552);
 if(auth_configured()&&ui_button(42+OX,382,168,32,"Nuevo QR",IC_NEXT,BS_ACCENT,0))auth_begin();
 if(auth_configured()&&ui_button(220+OX,382,190,32,"Cerrar sesion",IC_STOP,BS_NORMAL,0))auth_logout();
 if(auth_has_session()&&ui_button(42+OX,420,270,30,"Renovar sesion",IC_NEXT,BS_NORMAL,0))auth_refresh_now();
 int back=ui_button(436+OX,416,162,30,"Volver",IC_BACK,BS_NORMAL,0);ui_present();if(back)break;
 }
}
/* ---- rc8: selector de emotes del teclado del chat ----
 * Con sesion: Helix chat/emotes/global + chat/emotes?broadcaster_id (sin permisos nuevos;
 * los "solo suscriptores" del canal se marcan con candado porque sin user:read:emotes no se
 * sabe si estas suscrito). Sin Helix: GQL publico de la web. Mas los vistos en el chat.
 * Se inserta el nombre como texto, separado por espacios. */
#define TW_NEM 400
static const char *kb_chan_tw="";
static emo_item_t tw_em[TW_NEM];static int tw_n;static char tw_chan[64];
static int tw_has(const char*id){for(int i=0;i<tw_n;i++)if(!strcmp(tw_em[i].id,id))return 1;return 0;}
static void tw_add(const char*id,const char*name,int group,int locked,const char*note){
 if(tw_n>=TW_NEM||!id[0]||!name[0]||strlen(name)>=sizeof(tw_em[0].name)||strlen(id)>=sizeof(tw_em[0].id)||tw_has(id))return;
 for(const char*p=name;*p;p++)if((unsigned char)*p<33)return;
 emo_item_t*e=&tw_em[tw_n++];memset(e,0,sizeof(*e));snprintf(e->id,sizeof(e->id),"%s",id);snprintf(e->name,sizeof(e->name),"%s",name);e->group=group;e->locked=locked;snprintf(e->note,sizeof(e->note),"%s",note?note:"");}
static void tw_seen(void){static tchat_t m[24];int n=chat_get(m,24);for(int i=n-1;i>=0;i--)for(int k=0;k<m[i].ne;k++){char nm[40];int L=m[i].elen[k];if(L<=0||L>=40||m[i].eat[k]+L>(int)strlen(m[i].text))continue;memcpy(nm,m[i].text+m[i].eat[k],L);nm[L]=0;tw_add(m[i].eid[k],nm,2,0,"visto en el chat");}}
static int tw_helix_list(const char*json,int group,int chan){int n=0;const char*arr=json?jget(json,"data"):NULL;
 for(const char*e=arr?jarr_first(arr):NULL;e;e=jarr_next(e)){char id[48]="",nm[40]="",ty[24]="";const char*v;
  if((v=jget(e,"id")))jstr(v,id,sizeof(id));if((v=jget(e,"name")))jstr(v,nm,sizeof(nm));if((v=jget(e,"emote_type"))&&!jis_null(v))jstr(v,ty,sizeof(ty));
  int sub=chan&&!strcmp(ty,"subscriptions");tw_add(id,nm,group,sub,chan&&!strcmp(ty,"follower")?"seguidores":chan&&!strcmp(ty,"bitstier")?"bits":"");n++;}
 return n;}
static void tw_load(const char*channel){
 tw_n=0;snprintf(tw_chan,sizeof(tw_chan),"%s",channel?channel:"");
 sui_busy("Emotes","Cargando emotes del canal...");
 sh_resp_t r;char q[200];int got=0;
 snprintf(q,sizeof(q),"users?login=%s",tw_chan);
 if(auth_helix_get(q,&r)==200&&r.data){char bid[32]="";const char*a=jget(r.data,"data"),*u=a?jarr_first(a):NULL,*v=u?jget(u,"id"):NULL;if(v)jstr(v,bid,sizeof(bid));sh_resp_free(&r);
  if(bid[0]){snprintf(q,sizeof(q),"chat/emotes?broadcaster_id=%s",bid);if(auth_helix_get(q,&r)==200)got+=tw_helix_list(r.data,0,1);sh_resp_free(&r);}
  if(auth_helix_get("chat/emotes/global",&r)==200)got+=tw_helix_list(r.data,1,0);sh_resp_free(&r);
 }else sh_resp_free(&r);
 if(!got){   /* sin sesion valida para Helix: API publica de la web */
  char body[400],h0[80];snprintf(body,sizeof(body),"{\"query\":\"{emoteSet(id:\\\"0\\\"){emotes{id token}} user(login:\\\"%s\\\"){subscriptionProducts{emotes{id token}}}}\"}",tw_chan);
  snprintf(h0,sizeof(h0),"Client-Id: kimne78kx3ncx6brgo4mv6wki5h1ko");const char*h[]={h0,"Content-Type: application/json",NULL};
  sh_request("POST","https://gql.twitch.tv/gql",h,body,NULL,&r);
  if(sh_ok(&r)&&r.data){const char*p=jpath(r.data,"data.user.subscriptionProducts");
   for(const char*sp=p?jarr_first(p):NULL;sp;sp=jarr_next(sp)){const char*ea=jget(sp,"emotes");for(const char*e=ea?jarr_first(ea):NULL;e;e=jarr_next(e)){char id[48]="",nm[40]="";const char*v;if((v=jget(e,"id")))jstr(v,id,sizeof(id));if((v=jget(e,"token")))jstr(v,nm,sizeof(nm));tw_add(id,nm,0,1,"");}}
   p=jpath(r.data,"data.emoteSet.emotes");
   for(const char*e=p?jarr_first(p):NULL;e;e=jarr_next(e)){char id[48]="",nm[40]="";const char*v;if((v=jget(e,"id")))jstr(v,id,sizeof(id));if((v=jget(e,"token")))jstr(v,nm,sizeof(nm));tw_add(id,nm,1,0,"");}}
  sh_resp_free(&r);
 }
 sui_busy_end();
}
static int kb_emote(char*out,int cap){
 if(!tw_n||strcmp(tw_chan,kb_chan_tw))tw_load(kb_chan_tw);
 tw_seen();
 if(!tw_n){sui_message("Emotes","No se pudieron cargar emotes y todavia no aparecio ninguno en el chat.");return 0;}
 /* bloqueados al final de su grupo */
 static emo_item_t sorted[TW_NEM];int n=0;for(int pass=0;pass<2;pass++)for(int i=0;i<tw_n;i++)if(tw_em[i].locked==pass)sorted[n++]=tw_em[i];
 static const char*const groups[3]={"Canal","Globales","Vistos"};static int sel=0;char title[96];snprintf(title,sizeof(title),"Emotes - #%s",tw_chan);
 int k=emote_grid(title,sorted,n,groups,3,&sel);if(k<0)return 0;
 snprintf(out,cap,"%s",sorted[k].name);
 return 1;}
/* vista previa: las palabras que son emotes conocidos se dibujan como imagen */
static int kb_preview(const char*buf,int x,int y,int w){
 static tchat_t m;memset(&m,0,sizeof(m));const char*s=buf;size_t L=strlen(buf);if(L>190){s=buf+L-190;const char*q=strchr(s,' ');if(q)s=q+1;}
 snprintf(m.text,sizeof(m.text),"%s",s);int len=strlen(m.text);
 for(int i=0;i<len&&m.ne<8;){while(i<len&&m.text[i]==' ')i++;int j=i;while(j<len&&m.text[j]!=' ')j++;
  if(j>i&&j-i<40){for(int k=0;k<tw_n;k++)if((int)strlen(tw_em[k].name)==j-i&&!strncmp(tw_em[k].name,m.text+i,j-i)){m.eat[m.ne]=i;m.elen[m.ne]=j-i;snprintf(m.eid[m.ne],sizeof(m.eid[0]),"%s",tw_em[k].id);m.ne++;break;}}
  i=j;}
 if(!m.ne)return -1;
 return emote_preview_draw(&m,x,y,w);}
/* rc11: arma el tag "emotes" (indices en caracteres, como IRC) para el propio mensaje,
 * buscando cada palabra en los emotes conocidos (canal, globales, vistos en el chat). */
int kb_emote_tag(const char *text, char *tag, int cap){
 int q=0,len=strlen(text),ci=0,i=0,ne=0;tag[0]=0;
 tw_seen();
 while(i<len&&ne<8){
  while(i<len&&text[i]==' '){i++;ci++;}
  int j=i,cs=ci;
  while(j<len&&text[j]!=' '){unsigned char c=text[j];j+=c<0x80?1:(c&0xE0)==0xC0?2:(c&0xF0)==0xE0?3:4;ci++;}
  if(j>len)j=len;
  if(j>i&&j-i<40)for(int k=0;k<tw_n;k++)if((int)strlen(tw_em[k].name)==j-i&&!strncmp(tw_em[k].name,text+i,j-i)){
   int w=snprintf(tag+q,cap-q,"%s%s:%d-%d",q?"/":"",tw_em[k].id,cs,ci-1);if(w<0||w>=cap-q){tag[q]=0;return ne;}q+=w;ne++;break;}
  i=j;}
 return ne;}
#ifdef AUTOTEST_OWNEMOTE
/* rc11 prueba: el mismo camino que un envio real (tag calculado + eco local), sin mandar nada a Twitch */
void auth_test_own_echo(const char*channel,const char*text){
 static char etag[600];kb_chan_tw=channel;if(!tw_n||strcmp(tw_chan,kb_chan_tw))tw_load(kb_chan_tw);
 int ne=kb_emote_tag(text,etag,sizeof(etag));
 chat_local_echo("rc10_antes",text);            /* como quedaba antes: sin tag -> solo texto */
 chat_local_echo_e("rc11_ahora",text,etag);     /* ahora: con los rangos de los emotes */
 FILE*f=fopen("sd:/twitchwii-test/autotest.log","a");if(f){fprintf(f,"OWNEMOTE: %d emotes conocidos, texto='%s' tag='%s' (%d)\n",tw_n,text,etag,ne);fclose(f);}
}
#endif
void auth_compose(const char *channel,unsigned long broadcaster){
 if(!auth_ready()){ui_toast("Primero inicia sesion en Ajustes");return;}
 char text[351]="",hint[192];snprintf(hint,sizeof(hint),"Cuenta: %s / #%s. Enviar publica el mensaje. Max. 350 bytes.",auth_login(),channel);
#ifdef AUTH_MOCK
 auth_test_stage=4;
#endif
 kb_chan_tw=channel?channel:"";sui_kb_emote_hook=kb_emote;sui_kb_preview=kb_preview;
 int ok=sui_text_input_ex("Escribir en el chat",hint,text,350,"Enviar","Escribi tu mensaje");
 sui_kb_emote_hook=NULL;sui_kb_preview=NULL;
#ifdef AUTH_MOCK
 auth_test_stage=5;
#endif
 if(!ok)return;{size_t n=strlen(text);while(n&&text[n-1]==' ')text[--n]=0;}   /* rc8: sin el espacio final del selector */
 if(!auth_message_valid(text)){ui_toast("Mensaje vacio, muy largo o con caracteres de control. No enviado.");return;}
#ifdef WII_TWITCH_AUTH
 static char etag[600];if(!tw_n||strcmp(tw_chan,kb_chan_tw))tw_load(kb_chan_tw);kb_emote_tag(text,etag,sizeof(etag));
 if(!chat_send_text_e(text,etag))ui_toast("No enviado: chat conectando, ocupado o limite de frecuencia.");else ui_toast("En cola IRC. Un aviso del servidor puede rechazarlo.");
#else
 if(auth_kick_send(broadcaster,text)){chat_local_echo(auth_login(),text);ui_toast("Twitch acepto el mensaje");}else ui_toast(auth_status());
#endif
 memset(text,0,sizeof(text));
}
