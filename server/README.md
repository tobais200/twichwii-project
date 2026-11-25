# Servidor companion de TwitchWii

```
python3 twitchwii_server.py            # 0.0.0.0:8788, idioma es, datos reales de Twitch
python3 twitchwii_server.py --mock     # datos DEMO (sin tocar Twitch; los canales DEMO reproducen un testsrc de ffmpeg)
python3 twitchwii_server.py --play-mode transcode --tc-height 270 --tc-kbps 450
```

Requisitos: Python 3.8+. Opcionales: `pip install Pillow` (miniaturas/avatares para la Wii) y `ffmpeg` en el PATH
(modo transcode, VODs, canales DEMO).

## Opciones

| Opción | Descripción |
|---|---|
| `--host`, `--port` | dónde escuchar (def. `0.0.0.0:8788`) |
| `--lang` | idioma por defecto de los directos (`es`, `en`, `pt`… o `all`) |
| `--mock` | forzar datos DEMO |
| `--play-mode` | `proxy` (def.), `direct` o `transcode` para `/play/` si la Wii no pide uno |
| `--tc-height`, `--tc-kbps` | tamaño y bitrate del transcode (def. 270 / 450) |
| `--gql-client-id` / `TWITCH_GQL_CLIENT_ID` | Client-ID para gql.twitch.tv (def. el de la web) |
| `--oauth` / `TWITCH_OAUTH` | opcional: `OAuth xxx` de tu cuenta para GQL |
| `--helix-id`, `--helix-secret` | opcional: app de dev.twitch.tv (token client_credentials) para listados Helix |
| `--public-host` | IP a usar en URLs absolutas si estás detrás de NAT |
| `-v` | log detallado |

## Endpoints

JSON para la web: `/api/health`, `/api/live?lang=&limit=&after=&game=`, `/api/featured`, `/api/categories`,
`/api/channel/{login}[?playback=1]`, `/api/search?q=`, `/api/chat/{login}?since=N`.

Para la Wii (`/wii/...`, mismo contenido más imágenes ya escaladas en RGB24 y texto sin emojis):
`/wii/live`, `/wii/categories`, `/wii/channel/{login}`, `/wii/search`, `/wii/chat/{login}`, `/wii/token/{login}`.

Video: `/play/{login}.m3u8?mode=proxy|direct|transcode&max_h=360`, `/play/vod/{id}.m3u8[?t=segundos]`,
`/seg?u=` (proxy de segmentos de los CDN de Twitch), `/hls/{key}/...` (salida de ffmpeg).
Imágenes: `/img?u=&w=&h=&fmt=rgb|jpg`. En `/` sirve la preview web.

Límites: hasta 2 transcodes simultáneos (se cortan a los 45 s sin pedidos), hasta 4 salas de chat
(se cierran a los 90 s sin lectores) y 80 pedidos cada 10 s por IP.
