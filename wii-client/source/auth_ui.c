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
  wrap(270+OX,150,"Version sin configurar por el desarrollador. El desarrollador debe completar LOGIN-RC4F.md y recompilar. No tenes que crear una app ni ingresar IDs.",315,C_TEXT);
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
  wrap(282+OX,150,"Escanea, inicia sesion en Kick y aproba. Al finalizar volve a la Wii: no copies URLs ni codigos.",300,C_TEXT);
  wrap(282+OX,260,"El relay del desarrollador solo gestiona OAuth. El chat va directo a Kick.",300,C_DIM);
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
void auth_compose(const char *channel,unsigned long broadcaster){
 if(!auth_ready()){ui_toast("Primero inicia sesion en Ajustes");return;}
 char text[351]="",hint[192];snprintf(hint,sizeof(hint),"Cuenta: %s / #%s. Enviar publica el mensaje. Max. 350 bytes.",auth_login(),channel);
#ifdef AUTH_MOCK
 auth_test_stage=4;
#endif
 int ok=sui_text_input_ex("Escribir en el chat",hint,text,350,"Enviar","Escribi tu mensaje");
#ifdef AUTH_MOCK
 auth_test_stage=5;
#endif
 if(!ok)return;if(!auth_message_valid(text)){ui_toast("Mensaje vacio, muy largo o con caracteres de control. No enviado.");return;}
#ifdef WII_TWITCH_AUTH
 if(!chat_send_text(text))ui_toast("No enviado: chat conectando, ocupado o limite de frecuencia.");else ui_toast("En cola IRC. Un aviso del servidor puede rechazarlo.");
#else
 if(auth_kick_send(broadcaster,text)){chat_local_echo(auth_login(),text);ui_toast("Kick acepto el mensaje");}else ui_toast(auth_status());
#endif
 memset(text,0,sizeof(text));
}
