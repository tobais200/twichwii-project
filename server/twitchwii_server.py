#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TwitchWii companion server (NO OFICIAL, fan-made).

Servidor puente entre la Wii (o el preview web) y los datos PUBLICOS de twitch.tv:
  - directos en vivo (por idioma / categoria), categorias top, canal, busqueda, VODs
    via la API GraphQL publica de la web de Twitch (gql.twitch.tv, client-id web publico)
    o, opcionalmente, la API oficial Helix (si pasas --helix-id/--helix-secret)
  - miniaturas/avatares convertidos a RGB crudo para la Wii (o JPEG para el navegador)
  - video: token de reproduccion (PlaybackAccessToken) + lista de usher.ttvnw.net,
    en 3 modos: proxy HLS por HTTP plano (sin TLS en la Wii), transcodificacion con
    ffmpeg (270p ~450 kbps, TS) o redireccion directa
  - chat de solo lectura (IRC anonimo "justinfan", sin cuenta)
  - datos DEMO (mock) claramente marcados si Twitch no responde desde esta red

Solo biblioteca estandar de Python 3.8+. Opcionales: Pillow (imagenes), ffmpeg
(transcodificacion, VODs y video demo).

Uso:  python3 twitchwii_server.py [--host 0.0.0.0] [--port 8788] [--mock] [--lang es]
"""
import argparse
import collections
import io
import json
import os
import random
import re
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1.0.0"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/130.0 Safari/537.36")
HERE = os.path.dirname(os.path.abspath(__file__))
PREVIEW_DIR = os.path.normpath(os.path.join(HERE, "..", "preview"))

GQL_URL = "https://gql.twitch.tv/gql"
WEB_CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko"   # client-id publico de la web de twitch.tv
USHER_LIVE = "https://usher.ttvnw.net/api/channel/hls/{login}.m3u8"
USHER_VOD = "https://usher.ttvnw.net/vod/{vid}.m3u8"
HELIX = "https://api.twitch.tv/helix"
PAT_HASH = "0828119ded1c13477966434e15800ff57ddacf13ba1911c129dc2200705b0712"  # PlaybackAccessToken (persisted)

IMG_HOSTS = re.compile(r"(^|\.)(jtvnw\.net|twitch\.tv|twitchcdn\.net)$")
SEG_HOSTS = re.compile(r"(^|\.)(ttvnw\.net|jtvnw\.net|twitchcdn\.net|cloudfront\.net)$")
LOGIN_RE = re.compile(r"^[A-Za-z0-9_]{1,25}$")
VOD_RE = re.compile(r"^[0-9]{1,14}$")
GAMBLING = ("slot", "casino", "gambl", "poker", "roulette", "ruleta", "blackjack",
            "betting", "apuesta", "plinko", "crash", "dice", "baccarat", "sportsbook")

try:
    from PIL import Image, ImageDraw, ImageFont  # type: ignore
except Exception:  # pragma: no cover
    Image = None
FFMPEG = shutil.which("ffmpeg")
ARGS = None


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


# ------------------------------------------------------------------ upstream HTTP
class Upstream:
    """GET/POST a Twitch con limite de ritmo (educado) y cache en memoria."""

    def __init__(self, min_interval=0.2):
        self.min_interval = min_interval
        self.lock = threading.Lock()
        self.last = 0.0
        self.cache = {}
        self.clock = threading.Lock()
        self.stats = {"requests": 0, "errors": 0, "blocked": 0, "last_error": ""}

    def _pace(self):
        with self.lock:
            wait = self.last + self.min_interval - time.time()
            if wait > 0:
                time.sleep(wait)
            self.last = time.time()

    def request(self, url, data=None, headers=None, ttl=0, pace=True, timeout=15, accept="application/json"):
        key = url + "|" + (data.decode("utf-8", "replace") if data else "")
        if ttl:
            with self.clock:
                hit = self.cache.get(key)
                if hit and hit[0] > time.time():
                    return hit[1], hit[2]
        if pace:
            self._pace()
        self.stats["requests"] += 1
        h = {"User-Agent": UA, "Accept": accept, "Accept-Language": "es-AR,es;q=0.9,en;q=0.5"}
        h.update(headers or {})
        req = urllib.request.Request(url, data=data, headers=h, method="POST" if data is not None else "GET")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                status, body = r.status, r.read()
        except urllib.error.HTTPError as e:
            try:
                status, body = e.code, e.read()
            except Exception:
                status, body = e.code, b""
        except Exception as e:
            self.stats["last_error"] = str(e)[:200]
            status, body = 0, b""
        if status == 200 and ttl:
            with self.clock:
                if len(self.cache) > 800:
                    self.cache.clear()
                self.cache[key] = (time.time() + ttl, status, body)
        if status != 200:
            self.stats["errors"] += 1
            if status in (403, 429, 503):
                self.stats["blocked"] += 1
            self.stats["last_error"] = "HTTP %s %s" % (status, url[:80])
        return status, body

    def get(self, url, ttl=0, pace=False, accept="*/*", timeout=15):
        return self.request(url, ttl=ttl, pace=pace, accept=accept, timeout=timeout)


UP = Upstream()


def gql(query, variables=None, ttl=0):
    """POST a gql.twitch.tv. Devuelve (status, data|None, error_str)."""
    payload = {"query": query}
    if variables:
        payload["variables"] = variables
    headers = {"Client-Id": ARGS.gql_client_id, "Content-Type": "text/plain;charset=UTF-8"}
    if ARGS.oauth:
        headers["Authorization"] = "OAuth " + ARGS.oauth
    st, body = UP.request(GQL_URL, data=json.dumps(payload).encode("utf-8"), headers=headers, ttl=ttl)
    if st != 200:
        return st, None, "HTTP %s" % st
    try:
        js = json.loads(body.decode("utf-8"))
    except Exception:
        return 599, None, "respuesta no JSON"
    if js.get("errors") and not js.get("data"):
        return 500, None, str(js["errors"][0].get("message", "error gql"))[:160]
    return 200, js.get("data") or {}, ""


# ------------------------------------------------------------------ Helix (opcional, oficial)
class Helix:
    def __init__(self, cid, secret):
        self.cid, self.secret = cid, secret
        self.token, self.exp = "", 0
        self.lock = threading.Lock()

    def _tok(self):
        with self.lock:
            if self.token and self.exp > time.time() + 60:
                return self.token
            data = urllib.parse.urlencode({"client_id": self.cid, "client_secret": self.secret,
                                           "grant_type": "client_credentials"}).encode()
            st, body = UP.request("https://id.twitch.tv/oauth2/token", data=data,
                                  headers={"Content-Type": "application/x-www-form-urlencoded"})
            if st != 200:
                log("helix: no se pudo obtener token (HTTP %s)" % st)
                return ""
            js = json.loads(body.decode())
            self.token, self.exp = js["access_token"], time.time() + int(js.get("expires_in", 3600))
            return self.token

    def get(self, path, params, ttl=30):
        tok = self._tok()
        if not tok:
            return None
        url = HELIX + path + "?" + urllib.parse.urlencode(params, doseq=True)
        st, body = UP.request(url, headers={"Client-Id": self.cid, "Authorization": "Bearer " + tok}, ttl=ttl)
        if st != 200:
            return None
        return json.loads(body.decode())


HELIX_API = None


# ------------------------------------------------------------------ rate limit por cliente
class ClientLimiter:
    def __init__(self, per_10s=80):
        self.per = per_10s
        self.hits = {}
        self.lock = threading.Lock()

    def allow(self, ip):
        now = time.time()
        with self.lock:
            q = [t for t in self.hits.get(ip, []) if now - t < 10]
            if len(q) >= self.per:
                self.hits[ip] = q
                return False
            q.append(now)
            self.hits[ip] = q
            return True


LIMIT = ClientLimiter()

# ------------------------------------------------------------------ datos DEMO (mock)
MOCK_NAMES = [
    ("demo_ibai", "DemoIbai", "Just Chatting"),
    ("demo_auronplay", "DemoAuronPlay", "Grand Theft Auto V"),
    ("demo_rubius", "DemoRubius", "Minecraft"),
    ("demo_elxokas", "DemoElXokas", "World of Warcraft"),
    ("demo_thegrefg", "DemoTheGrefg", "Fortnite"),
    ("demo_illojuan", "DemoIlloJuan", "Just Chatting"),
    ("demo_spreen", "DemoSpreen", "Minecraft"),
    ("demo_coscu", "DemoCoscu", "Just Chatting"),
    ("demo_knekro", "DemoKnekro", "League of Legends"),
    ("demo_elmariana", "DemoElMariana", "Valorant"),
    ("demo_quackity", "DemoQuackity", "Minecraft"),
    ("demo_staxx", "DemoStaXx", "Counter-Strike"),
]
MOCK_COLORS = [(145, 70, 255), (191, 148, 255), (119, 44, 232), (235, 4, 0), (0, 173, 3),
               (30, 105, 255), (255, 107, 0), (250, 30, 140), (0, 180, 200), (90, 60, 200),
               (255, 200, 40), (20, 220, 140)]


def mock_stream(i):
    login, name, cat = MOCK_NAMES[i]
    return {
        "login": login, "name": name, "id": "mock-%d" % i,
        "title": "[DEMO] Directo de ejemplo #%d - datos simulados (Twitch no respondio)" % (i + 1),
        "viewers": 52000 // (i + 1) + 173 * i, "category": cat,
        "category_slug": cat.lower().replace(" ", "-"), "lang": "es", "mature": False,
        "start_time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 3600 * (i % 5 + 1) - 600 * i)),
        "thumb": "mock://thumb/%s" % login, "avatar": "mock://avatar/%s" % login,
        "tags": ["Español", "DEMO"], "mock": True,
    }


def mock_channel(login):
    for i, (s, name, cat) in enumerate(MOCK_NAMES):
        if s == login:
            ls = mock_stream(i)
            ls.update({"bio": "[DEMO] Canal simulado para probar la interfaz de TwitchWii.",
                       "banner": "mock://banner/%s" % s, "followers": 1500000 // (i + 1),
                       "partner": i % 2 == 0, "live": True, "videos": [], "source": "mock"})
            return ls
    return None


def mock_image(kind, slug, w, h):
    idx = sum(map(ord, slug)) % len(MOCK_COLORS)
    c = MOCK_COLORS[idx]
    if Image is None:
        return None
    im = Image.new("RGB", (w, h), (14, 14, 16))
    dr = ImageDraw.Draw(im)
    if kind == "avatar":
        im = Image.new("RGB", (w, h), c)
        dr = ImageDraw.Draw(im)
    else:
        for y in range(h):
            t = y / max(1, h - 1)
            col = tuple(int(c[k] * (0.7 - 0.5 * t) + 18 * t) for k in range(3))
            dr.line([(0, y), (w, y)], fill=col)
    label = slug.replace("demo_", "")[:2].upper()
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", max(12, h // 4))
    except Exception:
        f = None
    txt = label if kind == "avatar" else "DEMO " + label
    try:
        dr.text((w // 2, h // 2), txt, fill=(255, 255, 255), anchor="mm", font=f)
    except Exception:
        dr.text((4, 4), txt, fill=(255, 255, 255))
    return im


# ------------------------------------------------------------------ normalizacion
def is_gambling(*names):
    s = " ".join(n or "" for n in names).lower()
    return any(b in s for b in GAMBLING)


STREAM_FIELDS = ("id title viewersCount createdAt type previewImageURL(width:%d,height:%d) "
                 "freeformTags{name} game{name displayName slug} "
                 "broadcaster{login displayName profileImageURL(width:70) broadcastSettings{language isMature}}")


def compact_gql_stream(n):
    b = n.get("broadcaster") or {}
    g = n.get("game") or {}
    bs = b.get("broadcastSettings") or {}
    cat = g.get("displayName") or g.get("name") or ""
    return {
        "login": b.get("login", ""), "name": b.get("displayName") or b.get("login", ""),
        "id": n.get("id", ""), "title": n.get("title") or "",
        "viewers": int(n.get("viewersCount") or 0), "category": cat, "category_slug": g.get("slug") or "",
        "lang": (bs.get("language") or "").lower(), "mature": bool(bs.get("isMature")),
        "gambling": is_gambling(cat, g.get("slug")), "start_time": n.get("createdAt") or "",
        "thumb": n.get("previewImageURL") or "", "avatar": b.get("profileImageURL") or "",
        "tags": [t.get("name", "") for t in (n.get("freeformTags") or [])][:4], "mock": False,
    }


def fetch_live(lang="es", limit=24, after="", game=""):
    limit = max(1, min(int(limit), 60))
    if not ARGS.mock:
        langs = [] if (not lang or lang == "all") else [lang]
        fields = STREAM_FIELDS % (440, 248)
        if game:
            q = ("query($slug:String!,$n:Int,$a:Cursor,$langs:[String!]){game(slug:$slug){name displayName "
                 "streams(first:$n,after:$a,options:{languages:$langs}){edges{cursor node{%s}} pageInfo{hasNextPage}}}}" % fields)
            v = {"slug": game, "n": limit, "a": after or None, "langs": langs or None}
            st, d, err = gql(q, v, ttl=30)
            conn = ((d or {}).get("game") or {}).get("streams") if d else None
        else:
            q = ("query($n:Int,$a:Cursor,$langs:[Language!]){streams(first:$n,after:$a,options:{languages:$langs,sort:VIEWER_COUNT})"
                 "{edges{cursor node{%s}} pageInfo{hasNextPage}}}" % fields)
            v = {"n": limit, "a": after or None, "langs": [l.upper() for l in langs] or None}
            st, d, err = gql(q, v, ttl=30)
            conn = (d or {}).get("streams") if d else None
        if conn is not None:
            edges = conn.get("edges") or []
            items = [compact_gql_stream(e["node"]) for e in edges if e.get("node") and (e["node"].get("broadcaster") or {}).get("login")]
            nxt = edges[-1]["cursor"] if edges and (conn.get("pageInfo") or {}).get("hasNextPage", True) else ""
            return {"source": "twitch", "items": items, "next": nxt or ""}
        log("live: gql fallo (%s %s)" % (st, err))
        if HELIX_API and not game:
            r = helix_live(lang, limit, after)
            if r:
                return r
    items = [mock_stream(i) for i in range(min(limit, len(MOCK_NAMES)))]
    if game:
        items = [x for x in items if x["category_slug"] == game]
    return {"source": "mock", "items": items, "next": "",
            "notice": "DATOS DEMO: Twitch no respondio desde esta red (o se uso --mock)."}


def helix_live(lang, limit, after):
    p = {"first": limit}
    if lang and lang != "all":
        p["language"] = lang
    if after:
        p["after"] = after
    js = HELIX_API.get("/streams", p)
    if not js:
        return None
    ids = [s["user_id"] for s in js.get("data", [])]
    av = {}
    if ids:
        u = HELIX_API.get("/users", {"id": ids}, ttl=600) or {}
        av = {x["id"]: x.get("profile_image_url", "") for x in u.get("data", [])}
    items = []
    for s in js.get("data", []):
        items.append({"login": s["user_login"], "name": s["user_name"], "id": s["id"], "title": s.get("title", ""),
                      "viewers": s.get("viewer_count", 0), "category": s.get("game_name", ""), "category_slug": "",
                      "lang": s.get("language", ""), "mature": bool(s.get("is_mature")),
                      "gambling": is_gambling(s.get("game_name")), "start_time": s.get("started_at", ""),
                      "thumb": s.get("thumbnail_url", "").replace("{width}", "440").replace("{height}", "248"),
                      "avatar": av.get(s["user_id"], ""), "tags": (s.get("tags") or [])[:4], "mock": False})
    return {"source": "twitch-helix", "items": items, "next": (js.get("pagination") or {}).get("cursor", "")}


def fetch_categories(limit=24):
    if not ARGS.mock:
        q = "query($n:Int){games(first:$n){edges{node{id name displayName slug viewersCount boxArtURL(width:144,height:192)}}}}"
        st, d, err = gql(q, {"n": max(1, min(int(limit), 50))}, ttl=120)
        if d and d.get("games"):
            items = []
            for e in d["games"].get("edges") or []:
                n = e.get("node") or {}
                items.append({"name": n.get("displayName") or n.get("name", ""), "slug": n.get("slug", ""),
                              "viewers": int(n.get("viewersCount") or 0), "boxart": n.get("boxArtURL") or "",
                              "gambling": is_gambling(n.get("name"), n.get("slug"))})
            return {"source": "twitch", "items": items}
        log("categories: gql fallo (%s %s)" % (st, err))
    seen, items = set(), []
    for _l, _n, cat in MOCK_NAMES:
        if cat not in seen:
            seen.add(cat)
            items.append({"name": cat, "slug": cat.lower().replace(" ", "-"), "viewers": 10000 * (8 - len(items)),
                          "boxart": "mock://boxart/" + cat.lower().replace(" ", "_"), "gambling": False})
    return {"source": "mock", "items": items}


CHANNEL_Q = ("query($l:String!){user(login:$l){id login displayName description profileImageURL(width:150) "
             "bannerImageURL offlineImageURL followers{totalCount} roles{isPartner isAffiliate} "
             "broadcastSettings{title language isMature game{name displayName slug}} "
             "stream{id title viewersCount createdAt type freeformTags{name} game{name displayName slug} previewImageURL(width:640,height:360)} "
             "lastBroadcast{title startedAt game{displayName}} "
             "videos(first:8,type:ARCHIVE,sort:TIME){edges{node{id title lengthSeconds createdAt viewCount "
             "previewThumbnailURL(width:320,height:180) game{displayName}}}}}}")


def fetch_channel(login):
    """(status, compact) -- compact incluye videos[]"""
    if not ARGS.mock:
        st, d, err = gql(CHANNEL_Q, {"l": login}, ttl=10)
        if d is not None:
            u = d.get("user")
            if not u:
                m = mock_channel(login)
                return (200, m) if m else (404, None)
            return 200, compact_user(u)
        log("channel %s: gql fallo (%s %s)" % (login, st, err))
    m = mock_channel(login)
    if m:
        return 200, m
    return (404, None) if ARGS.mock else (502, None)


def compact_user(u):
    s = u.get("stream") or None
    bs = u.get("broadcastSettings") or {}
    g = (s or {}).get("game") or bs.get("game") or {}
    cat = g.get("displayName") or g.get("name") or ""
    roles = u.get("roles") or {}
    out = {
        "login": u.get("login", ""), "name": u.get("displayName") or u.get("login", ""),
        "bio": u.get("description") or "", "avatar": u.get("profileImageURL") or "",
        "banner": u.get("bannerImageURL") or u.get("offlineImageURL") or "",
        "offline_img": u.get("offlineImageURL") or "",
        "followers": int((u.get("followers") or {}).get("totalCount") or 0),
        "partner": bool(roles.get("isPartner")), "affiliate": bool(roles.get("isAffiliate")),
        "live": bool(s and s.get("type") == "live"), "title": (s or {}).get("title") or bs.get("title") or "",
        "viewers": int((s or {}).get("viewersCount") or 0), "category": cat, "category_slug": g.get("slug") or "",
        "lang": (bs.get("language") or "").lower(), "mature": bool(bs.get("isMature")),
        "gambling": is_gambling(cat, g.get("slug")), "start_time": (s or {}).get("createdAt") or "",
        "thumb": (s or {}).get("previewImageURL") or u.get("offlineImageURL") or u.get("bannerImageURL") or "",
        "tags": [t.get("name", "") for t in ((s or {}).get("freeformTags") or [])][:4],
        "last_broadcast": ((u.get("lastBroadcast") or {}).get("startedAt") or ""),
        "videos": [], "source": "twitch", "mock": False,
    }
    for e in ((u.get("videos") or {}).get("edges") or []):
        n = e.get("node") or {}
        out["videos"].append({"id": n.get("id", ""), "title": n.get("title") or "", "length": int(n.get("lengthSeconds") or 0),
                              "created": n.get("createdAt") or "", "views": int(n.get("viewCount") or 0),
                              "thumb": n.get("previewThumbnailURL") or "",
                              "category": (n.get("game") or {}).get("displayName") or ""})
    return out


def fetch_search(q):
    if not ARGS.mock:
        query = ("query($q:String!){searchFor(userQuery:$q,platform:\"web\"){"
                 "channels{items{login displayName followers{totalCount} profileImageURL(width:70) roles{isPartner} "
                 "stream{id viewersCount game{displayName}} broadcastSettings{title}}} "
                 "games{items{name displayName slug viewersCount boxArtURL(width:144,height:192)}}}}")
        st, d, err = gql(query, {"q": q}, ttl=60)
        if d and d.get("searchFor") is not None:
            sf = d["searchFor"] or {}
            chans = []
            for c in ((sf.get("channels") or {}).get("items") or [])[:30]:
                s = c.get("stream") or None
                chans.append({"login": c.get("login", ""), "name": c.get("displayName") or c.get("login", ""),
                              "live": bool(s), "viewers": int((s or {}).get("viewersCount") or 0),
                              "category": ((s or {}).get("game") or {}).get("displayName") or "",
                              "title": (c.get("broadcastSettings") or {}).get("title") or "",
                              "followers": int((c.get("followers") or {}).get("totalCount") or 0),
                              "avatar": c.get("profileImageURL") or "", "partner": bool((c.get("roles") or {}).get("isPartner"))})
            chans.sort(key=lambda c: (not c["live"], -c["followers"]))
            games = [{"name": g.get("displayName") or g.get("name", ""), "slug": g.get("slug", ""),
                      "viewers": int(g.get("viewersCount") or 0), "boxart": g.get("boxArtURL") or ""}
                     for g in ((sf.get("games") or {}).get("items") or [])[:10]]
            return {"source": "twitch", "channels": chans, "categories": games}
        log("search: gql fallo (%s %s)" % (st, err))
    ql = q.lower()
    chans = [{"login": s, "name": n, "live": True, "viewers": 1000, "category": c, "title": "[DEMO]", "followers": 1000,
              "avatar": "mock://avatar/" + s, "partner": False}
             for s, n, c in MOCK_NAMES if ql in s or ql in n.lower()]
    return {"source": "mock", "channels": chans, "categories": [],
            "notice": "DATOS DEMO: Twitch no respondio desde esta red (o se uso --mock)."}


# ------------------------------------------------------------------ token de reproduccion + usher
def playback_token(login=None, vod=None):
    kind = "streamPlaybackAccessToken" if login else "videoPlaybackAccessToken"
    arg = 'channelName:"%s"' % login if login else 'id:"%s"' % vod
    q = 'query{%s(%s,params:{platform:"web",playerBackend:"mediaplayer",playerType:"site"}){value signature}}' % (kind, arg)
    st, d, err = gql(q)
    tok = (d or {}).get(kind) if d else None
    if not tok:
        # respaldo: consulta "persisted" que usa la web
        body = {"operationName": "PlaybackAccessToken", "extensions": {"persistedQuery": {"version": 1, "sha256Hash": PAT_HASH}},
                "variables": {"isLive": bool(login), "login": login or "", "isVod": bool(vod), "vodID": vod or "",
                              "playerType": "site", "platform": "web"}}
        headers = {"Client-Id": ARGS.gql_client_id}
        if ARGS.oauth:
            headers["Authorization"] = "OAuth " + ARGS.oauth
        st2, raw = UP.request(GQL_URL, data=json.dumps(body).encode(), headers=headers)
        try:
            dd = json.loads(raw.decode()).get("data") or {}
            tok = dd.get(kind)
        except Exception:
            tok = None
        if not tok:
            return None, "token de reproduccion: %s" % (err or "HTTP %s" % st2)
    return tok, ""


def usher_url(login=None, vod=None):
    tok, err = playback_token(login, vod)
    if not tok:
        return None, err
    try:
        if json.loads(tok["value"]).get("authorization", {}).get("forbidden"):
            return None, "Twitch no permite ver este canal sin iniciar sesion (region/suscriptores)."
    except Exception:
        pass
    p = {"sig": tok["signature"], "token": tok["value"], "allow_source": "true", "allow_audio_only": "true",
         "fast_bread": "true", "p": str(random.randint(100000, 9999999)), "player_backend": "mediaplayer",
         "playlist_include_framerate": "true", "supported_codecs": "h264", "cdm": "wv"}
    base = USHER_LIVE.format(login=login) if login else USHER_VOD.format(vid=vod)
    return base + "?" + urllib.parse.urlencode(p), ""


# ------------------------------------------------------------------ imagenes
IMG_CACHE = collections.OrderedDict()
IMG_LOCK = threading.Lock()


def load_image(src):
    if src.startswith("mock://"):
        _, _, rest = src.partition("mock://")
        kind, _, slug = rest.partition("/")
        sz = {"avatar": (300, 300), "boxart": (144, 192)}.get(kind, (640, 360))
        return mock_image(kind, slug, *sz)
    p = urllib.parse.urlparse(src)
    if p.scheme != "https" or not IMG_HOSTS.search(p.hostname or ""):
        return None
    st, body = UP.get(src, ttl=120 if "previews-ttv" in src else 900, accept="image/*,*/*")
    if st != 200 or not body:
        return None
    if Image is not None:
        try:
            return Image.open(io.BytesIO(body)).convert("RGB")
        except Exception:
            return None
    return body


def render_image(src, w, h, fmt):
    key = (src, w, h, fmt)
    with IMG_LOCK:
        if key in IMG_CACHE:
            IMG_CACHE.move_to_end(key)
            return IMG_CACHE[key]
    im = load_image(src)
    if im is None:
        return None
    data = None
    if Image is not None and not isinstance(im, (bytes, bytearray)):
        sw, sh = im.size
        sc = max(w / sw, h / sh)
        nw, nh = max(w, int(sw * sc + 0.5)), max(h, int(sh * sc + 0.5))
        im = im.resize((nw, nh), Image.BILINEAR)
        x0, y0 = (nw - w) // 2, (nh - h) // 2
        im = im.crop((x0, y0, x0 + w, y0 + h))
        if fmt == "rgb":
            data = b"KWI1" + w.to_bytes(2, "big") + h.to_bytes(2, "big") + im.tobytes()
        else:
            b = io.BytesIO()
            im.save(b, "JPEG", quality=82)
            data = b.getvalue()
    elif FFMPEG:
        vf = "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d" % (w, h, w, h)
        pix = ["-f", "rawvideo", "-pix_fmt", "rgb24"] if fmt == "rgb" else ["-f", "mjpeg"]
        try:
            out = subprocess.run([FFMPEG, "-loglevel", "error", "-i", "pipe:0", "-vf", vf, "-frames:v", "1"] + pix + ["pipe:1"],
                                 input=im, capture_output=True, timeout=10).stdout
        except Exception:
            out = b""
        if out:
            data = (b"KWI1" + w.to_bytes(2, "big") + h.to_bytes(2, "big") + out) if fmt == "rgb" else out
    if data:
        with IMG_LOCK:
            IMG_CACHE[key] = data
            while len(IMG_CACHE) > 500:
                IMG_CACHE.popitem(last=False)
    return data


# ------------------------------------------------------------------ HLS: variantes, proxy, transcodificacion
VARIANT_CACHE = {}


def parse_master(text, base):
    out, pend = [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXT-X-STREAM-INF"):
            pend = line
        elif pend and line and not line.startswith("#"):
            m = re.search(r"RESOLUTION=(\d+)x(\d+)", pend)
            b = re.search(r"BANDWIDTH=(\d+)", pend)
            fr = re.search(r"FRAME-RATE=([\d.]+)", pend)
            out.append((int(m.group(2)) if m else 0, int(b.group(1)) if b else 0, urllib.parse.urljoin(base, line),
                        float(fr.group(1)) if fr else 30.0))
            pend = None
    return out


def pick_variant(key, max_h=360, force=False):
    """key = login o 'vod_<id>'. Devuelve (url_variante|demo://, info|error)."""
    now = time.time()
    hit = VARIANT_CACHE.get((key, max_h))
    if hit and hit[0] > now and not force:
        return hit[1], hit[2]
    if key.startswith("demo_"):
        return "demo://testsrc", "demo"
    if ARGS.mock:
        return None, "modo --mock: solo canales DEMO"
    if key.startswith("vod_"):
        master, err = usher_url(vod=key[4:])
    else:
        master, err = usher_url(login=key)
    if not master:
        return None, err
    st, body = UP.get(master)
    if st == 404:
        return None, "el canal no esta en vivo"
    if st != 200:
        return None, "lista maestra HTTP %s" % st
    vs = [v for v in parse_master(body.decode("utf-8", "replace"), master) if v[0]]
    ok = [v for v in vs if v[0] <= max_h]
    if ok:
        best = max(ok, key=lambda x: (x[0], -x[3]))      # mayor alto; a igual alto, 30 fps antes que 60
    elif vs:
        best = min(vs, key=lambda x: x[0])
    else:
        return None, "sin variantes de video"
    info = "%dp%d %d kbps" % (best[0], int(best[3]), best[1] // 1000)
    VARIANT_CACHE[(key, max_h)] = (now + 300, best[2], info, master)
    return best[2], info


def master_for(key):
    for (k, _h), v in list(VARIANT_CACHE.items()):
        if k == key and v[0] > time.time():
            return v[3]
    return None


def rewrite_media(text, base, host):
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s and not s.startswith("#"):
            out.append("http://%s/seg?u=%s" % (host, urllib.parse.quote(urllib.parse.urljoin(base, s), safe="")))
        elif s.startswith("#EXT-X-MAP:"):
            m = re.search(r'URI="([^"]+)"', s)
            if m:
                absu = urllib.parse.urljoin(base, m.group(1))
                s = s.replace(m.group(1), "http://%s/seg?u=%s" % (host, urllib.parse.quote(absu, safe="")))
            out.append(s)
        elif s.startswith("#EXT-X-DATERANGE") or s.startswith("#EXT-X-TWITCH-PREFETCH"):
            continue      # la Wii no los usa; aliviana la lista
        else:
            out.append(line)
    return "\n".join(out) + "\n"


class Transcoder:
    """un ffmpeg por canal/VOD -> HLS liviano (TS) en un directorio temporal; se apaga solo si nadie mira."""

    def __init__(self, max_jobs=2, idle=45):
        self.jobs = {}
        self.lock = threading.Lock()
        self.max_jobs = max_jobs
        self.idle = idle
        self.root = tempfile.mkdtemp(prefix="twitchwii-hls-")
        threading.Thread(target=self._reaper, daemon=True).start()

    def _reaper(self):
        while True:
            time.sleep(5)
            with self.lock:
                for k, j in list(self.jobs.items()):
                    if time.time() - j["touch"] > self.idle or j["proc"].poll() is not None:
                        self._kill(k)

    def _kill(self, key):
        j = self.jobs.pop(key, None)
        if not j:
            return
        try:
            j["proc"].terminate()
            j["proc"].wait(timeout=3)
        except Exception:
            try:
                j["proc"].kill()
            except Exception:
                pass
        shutil.rmtree(j["dir"], ignore_errors=True)
        log("transcode %s: detenido" % key)

    def touch(self, key):
        with self.lock:
            j = self.jobs.get(key)
            if j:
                j["touch"] = time.time()
                return j["dir"]
        return None

    def start(self, key, src, height=270, vbit=450, realtime=False, seek=0):
        with self.lock:
            j = self.jobs.get(key)
            if j and j["proc"].poll() is None and j["seek"] == seek:
                j["touch"] = time.time()
                return j["dir"], None
            if j:
                self._kill(key)
            if len(self.jobs) >= self.max_jobs:
                self._kill(min(self.jobs, key=lambda s: self.jobs[s]["touch"]))
            d = os.path.join(self.root, key)
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d, exist_ok=True)
            if src == "demo://testsrc":
                inp = ["-re", "-f", "lavfi", "-i", "testsrc2=size=480x270:rate=25",
                       "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"]
                amap = "1:a:0"
            else:
                inp = (["-re"] if realtime else []) + (["-ss", str(seek)] if seek else []) + [
                    "-user_agent", UA, "-reconnect", "1", "-reconnect_streamed", "1", "-i", src]
                amap = "0:a:0?"
            cmd = [FFMPEG, "-hide_banner", "-loglevel", "warning"] + inp + [
                "-map", "0:v:0", "-map", amap,
                "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-profile:v", "main",
                "-pix_fmt", "yuv420p", "-vf", "scale=-2:%d" % height, "-r", "25", "-g", "50",
                "-b:v", "%dk" % vbit, "-maxrate", "%dk" % int(vbit * 1.2), "-bufsize", "%dk" % (vbit * 2),
                "-c:a", "aac", "-b:a", "96k", "-ac", "2", "-ar", "48000",
                "-f", "hls", "-hls_time", "2", "-hls_list_size", "6",
                "-hls_flags", "delete_segments+omit_endlist+independent_segments",
                "-hls_segment_filename", os.path.join(d, "seg%05d.ts"), os.path.join(d, "index.m3u8")]
            try:
                p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except Exception as e:
                return None, "no se pudo lanzar ffmpeg: %s" % e
            self.jobs[key] = {"proc": p, "dir": d, "touch": time.time(), "src": src, "seek": seek}
            log("transcode %s: ffmpeg iniciado (%dp %d kbps%s)" % (key, height, vbit, ", VOD" if realtime else ""))
        return d, None

    def status(self):
        with self.lock:
            return {k: {"alive": j["proc"].poll() is None, "idle_s": int(time.time() - j["touch"])} for k, j in self.jobs.items()}


TRANS = None


# ------------------------------------------------------------------ chat (solo lectura, IRC anonimo)
class ChatRoom:
    def __init__(self, login):
        self.login = login
        self.msgs = collections.deque(maxlen=150)
        self.seq = 0
        self.touch = time.time()
        self.status = "conectando"
        self.lock = threading.Lock()
        self.stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def add(self, name, color, text, system=False):
        with self.lock:
            self.seq += 1
            self.msgs.append({"id": self.seq, "name": name, "color": color, "text": text[:300], "sys": system,
                              "ts": int(time.time())})

    def _connect(self):
        host = "irc.chat.twitch.tv"
        last = None
        for port, use_tls in ((6697, True), (443, True), (6667, False)):
            try:
                raw = socket.create_connection((host, port), 8)
                s = ssl.create_default_context().wrap_socket(raw, server_hostname=host) if use_tls else raw
                s.sendall(("CAP REQ :twitch.tv/tags twitch.tv/commands\r\nPASS SCHMOOPIIE\r\nNICK justinfan%d\r\n"
                           "JOIN #%s\r\n" % (random.randint(10000, 99999), self.login)).encode())
                s.settimeout(5)
                first = s.recv(4096)
                if not first or first.startswith(b"HTTP/"):
                    s.close()
                    raise OSError("respuesta no IRC en puerto %d" % port)
                return s, first, port
            except Exception as e:
                last = e
        raise OSError(str(last))

    def _run(self):
        if self.login.startswith("demo_"):
            return self._run_mock()
        backoff = 2
        while not self.stop:
            try:
                s, buf, port = self._connect()
                self.status = "conectado (puerto %d)" % port
                self.add("", "", "Conectado al chat de %s (solo lectura)" % self.login, True)
                backoff = 2
                s.settimeout(30)
                while not self.stop:
                    while b"\r\n" in buf:
                        line, buf = buf.split(b"\r\n", 1)
                        self._line(s, line.decode("utf-8", "replace"))
                    if time.time() - self.touch > 90:
                        self.stop = True
                        break
                    try:
                        chunk = s.recv(8192)
                    except socket.timeout:
                        s.sendall(b"PING :tmi.twitch.tv\r\n")
                        continue
                    if not chunk:
                        raise OSError("conexion cerrada")
                    buf += chunk
                s.close()
            except Exception as e:
                self.status = "error: %s" % e
                if self.stop:
                    break
                time.sleep(backoff)
                backoff = min(backoff * 2, 30)

    def _line(self, s, line):
        if line.startswith("PING"):
            s.sendall(b"PONG :tmi.twitch.tv\r\n")
            return
        tags = {}
        if line.startswith("@"):
            t, _, line = line.partition(" ")
            for kv in t[1:].split(";"):
                k, _, v = kv.partition("=")
                tags[k] = v
        m = re.match(r"^:([^! ]+)(?:![^ ]+)? (PRIVMSG|USERNOTICE) #\S+(?: :(.*))?$", line)
        if not m:
            return
        name = tags.get("display-name") or m.group(1)
        text = m.group(3) or ""
        if text.startswith("\x01ACTION "):
            text = text[8:].rstrip("\x01")
        if m.group(2) == "USERNOTICE":
            sysm = tags.get("system-msg", "").replace("\\s", " ")
            self.add("", "", (sysm + (" - " + text if text else "")).strip(), True)
        else:
            self.add(name, tags.get("color", ""), text)

    def _run_mock(self):
        self.status = "demo"
        lines = ["hola chat!", "KEKW", "que buen directo", "saludos desde Argentina", "PogChamp", "jajaja",
                 "primera vez por aca", "LUL", "GG", "Wii >>> PC"]
        names = ["wiifan64", "mario_ok", "Luigi2008", "PeachTV", "Kirbito", "zelda_ar", "samus99"]
        self.add("", "", "Chat DEMO (simulado)", True)
        while not self.stop and time.time() - self.touch < 90:
            self.add(random.choice(names), random.choice(["#9146FF", "#FF7F50", "#1E90FF", "#00FF7F", ""]), random.choice(lines))
            time.sleep(random.uniform(0.6, 2.5))

    def since(self, sid, limit=30):
        self.touch = time.time()
        with self.lock:
            out = [m for m in self.msgs if m["id"] > sid]
        return out[-limit:]


class ChatHub:
    def __init__(self, max_rooms=4):
        self.rooms = {}
        self.lock = threading.Lock()
        self.max_rooms = max_rooms

    def get(self, login):
        with self.lock:
            for k, r in list(self.rooms.items()):
                if r.stop or time.time() - r.touch > 90:
                    r.stop = True
                    del self.rooms[k]
            r = self.rooms.get(login)
            if not r:
                if len(self.rooms) >= self.max_rooms:
                    old = min(self.rooms, key=lambda k: self.rooms[k].touch)
                    self.rooms.pop(old).stop = True
                r = self.rooms[login] = ChatRoom(login)
            return r

    def status(self):
        return {k: r.status for k, r in self.rooms.items()}


CHAT = ChatHub()


# ------------------------------------------------------------------ HTTP
def wii_clean(o):
    """la Wii solo dibuja Latin-1: quitar emojis/otros y espacios repetidos."""
    if isinstance(o, str):
        t = "".join(ch if ord(ch) < 256 else " " for ch in o)
        return re.sub(r"\s{2,}", " ", t).strip()
    if isinstance(o, list):
        return [wii_clean(x) for x in o]
    if isinstance(o, dict):
        return {k: (wii_clean(v) if k not in ("thumb_rgb", "avatar_rgb", "banner_rgb", "boxart_rgb") else v) for k, v in o.items()}
    return o


def q_img(u, w, h):
    return "/img?w=%d&h=%d&fmt=rgb&u=%s" % (w, h, urllib.parse.quote(u or "", safe=""))


class Handler(BaseHTTPRequestHandler):
    server_version = "TwitchWii/" + VERSION
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        if ARGS.verbose:
            log("%s %s" % (self.client_address[0], fmt % a))

    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def _json(self, obj, code=200):
        if self.path.startswith("/wii/"):
            obj = wii_clean(obj)
        self._send(code, json.dumps(obj, ensure_ascii=True))   # \uXXXX: la Wii los pasa a Latin-1

    def _err(self, code, msg):
        self._json({"error": msg, "code": code}, code)

    def do_OPTIONS(self):
        self._send(204, b"")

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[-1] for k, v in urllib.parse.parse_qs(u.query).items()}
        heavy = u.path.startswith(("/api/", "/wii/", "/play/"))
        if heavy and not u.path.startswith(("/api/chat", "/wii/chat")) and not LIMIT.allow(self.client_address[0]):
            return self._err(429, "Demasiados pedidos; espera unos segundos.")
        try:
            return self._route(u.path, q)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            log("error en %s: %r" % (u.path, e))
            try:
                self._err(500, "error interno: %s" % e)
            except Exception:
                pass

    def _host(self):
        return self.headers.get("Host") or "%s:%d" % (ARGS.public_host or "127.0.0.1", ARGS.port)

    def _route(self, path, q):
        lang = q.get("lang", ARGS.lang)
        wii = path.startswith("/wii/")
        if path in ("/api/health", "/health", "/wii/health"):
            return self._json({
                "ok": True, "name": "TwitchWii companion", "version": VERSION, "mock_forced": ARGS.mock,
                "ffmpeg": bool(FFMPEG), "pillow": Image is not None, "helix": bool(HELIX_API), "oauth": bool(ARGS.oauth),
                "upstream": UP.stats, "transcodes": TRANS.status() if TRANS else {}, "chat": CHAT.status(),
                "time": int(time.time()),
            })
        if path in ("/api/live", "/api/livestreams", "/wii/live"):
            r = fetch_live(lang, q.get("limit", "24"), q.get("after", ""), q.get("game", ""))
            if wii:
                tw, th = int(q.get("tw", "224")), int(q.get("th", "126"))
                for c in r["items"]:
                    c["thumb_rgb"] = q_img(c["thumb"], tw, th)
                    c["avatar_rgb"] = q_img(c["avatar"], 28, 28)
                    c["slug"] = c["login"]
            return self._json(r)
        if path in ("/api/featured", "/wii/featured"):
            r = fetch_live(lang, q.get("limit", "6"))
            return self._json({"source": r["source"], "items": r["items"][:int(q.get("limit", "6"))]})
        if path in ("/api/categories", "/wii/categories"):
            r = fetch_categories(q.get("limit", "24"))
            if wii:
                bw, bh = int(q.get("bw", "96")), int(q.get("bh", "128"))
                for c in r["items"]:
                    c["boxart_rgb"] = q_img(c["boxart"], bw, bh)
            return self._json(r)
        m = re.match(r"^/(api|wii)/channels?/([^/]+)$", path)
        if m:
            login = m.group(2).lower()
            if not LOGIN_RE.match(login) and not login.startswith("demo_"):
                return self._err(400, "nombre de canal invalido")
            st, c = fetch_channel(login)
            if st != 200:
                return self._err(st, "No existe ese canal." if st == 404 else "Twitch no respondio (HTTP %s)." % st)
            c = dict(c)
            c["slug"] = c["login"]
            if wii:
                c["thumb_rgb"] = q_img(c["thumb"] or c["banner"], 304, 171)
                c["banner_rgb"] = q_img(c["banner"] or c["thumb"], 640, 110)
                c["avatar_rgb"] = q_img(c["avatar"], 64, 64)
                vids = []
                for v in c["videos"][:6]:
                    v = dict(v)
                    v["thumb_rgb"] = q_img(v["thumb"], 144, 81)
                    vids.append(v)
                c["videos"] = vids
            elif q.get("playback") == "1" and c.get("live"):
                c["playback_url"], _ = usher_url(login=login) if not c.get("mock") else ("", "")
            c["play"] = "/play/%s.m3u8" % login
            return self._json(c)
        if path in ("/api/search", "/wii/search"):
            term = (q.get("q") or "").strip()[:40]
            if not term:
                return self._err(400, "falta q")
            r = fetch_search(term)
            if wii:
                for c in r["channels"]:
                    c["avatar_rgb"] = q_img(c["avatar"], 40, 40)
                    c["slug"] = c["login"]
                for g in r["categories"]:
                    g["boxart_rgb"] = q_img(g["boxart"], 72, 96)
            return self._json(r)
        m = re.match(r"^/(api|wii)/chat/([A-Za-z0-9_]{1,25})$", path)
        if m:
            room = CHAT.get(m.group(2).lower())
            try:
                sid = int(q.get("since", "0"))
            except ValueError:
                sid = 0
            msgs = room.since(sid, int(q.get("limit", "30")))
            return self._json({"status": room.status, "last": room.seq, "messages": msgs})
        # token renovado para el reproductor de la Wii en modo directo (forma {"playback_url": ...})
        m = re.match(r"^/wii/token/([A-Za-z0-9_]{1,25})$", path)
        if m:
            url, err = usher_url(login=m.group(1).lower())
            if not url:
                return self._err(404, err)
            return self._json({"playback_url": url, "livestream": {"is_live": True}, "slug": m.group(1).lower()})
        if path == "/img":
            src = q.get("u", "")
            try:
                w = max(8, min(int(q.get("w", "224")), 640))
                h = max(8, min(int(q.get("h", "126")), 360))
            except ValueError:
                return self._err(400, "w/h invalidos")
            fmt = "rgb" if q.get("fmt") == "rgb" else "jpg"
            if not src:
                return self._err(404, "sin imagen")
            data = render_image(src, w, h, fmt)
            if not data:
                return self._err(404, "imagen no disponible")
            return self._send(200, data, "application/octet-stream" if fmt == "rgb" else "image/jpeg",
                              {"Cache-Control": "max-age=120"})
        m = re.match(r"^/play/vod/([0-9]{1,14})\.m3u8$", path)
        if m:
            return self._play("vod_" + m.group(1), q.get("mode", "transcode"), q)
        m = re.match(r"^/play/([A-Za-z0-9_]{1,25})\.m3u8$", path)
        if m:
            return self._play(m.group(1).lower(), q.get("mode", ARGS.play_mode), q)
        if path == "/seg":
            return self._seg(q.get("u", ""))
        m = re.match(r"^/hls/([A-Za-z0-9_]{1,40})/([A-Za-z0-9_.]+)$", path)
        if m:
            return self._hls_file(m.group(1), m.group(2))
        return self._static(path)

    def _play(self, key, mode, q):
        try:
            max_h = int(q.get("max_h", "360"))
        except ValueError:
            max_h = 360
        vod = key.startswith("vod_")
        if key.startswith("demo_"):
            mode = "transcode" if FFMPEG else mode
        if vod and mode == "proxy":
            mode = "transcode"      # el reproductor de la Wii es "en vivo": los VODs se emiten en tiempo real con ffmpeg
        if mode == "transcode":
            if not FFMPEG:
                return self._err(501, "ffmpeg no esta instalado en el servidor; usa mode=proxy o mode=direct.")
            src, info = pick_variant(key, max_h=480)
            if not src:
                return self._err(404, info)
            try:
                seek = max(0, int(q.get("t", "0")))
            except ValueError:
                seek = 0
            d, e = TRANS.start(key, src, int(q.get("h", ARGS.tc_height)), int(q.get("kbps", ARGS.tc_kbps)),
                               realtime=vod, seek=seek if vod else 0)
            if not d:
                return self._err(500, e)
            idx = os.path.join(d, "index.m3u8")
            t0 = time.time()
            while time.time() - t0 < 25:
                try:
                    with open(idx) as f:
                        if f.read().count("#EXTINF") >= 2:
                            break
                except OSError:
                    pass
                time.sleep(0.3)
            else:
                return self._err(504, "ffmpeg no produjo video a tiempo")
            return self._send(302, b"", "text/plain", {"Location": "http://%s/hls/%s/index.m3u8" % (self._host(), key)})
        src, info = pick_variant(key, max_h=max_h)
        if not src:
            return self._err(404, info)
        if src == "demo://testsrc":
            return self._err(409, "canal DEMO: usa mode=transcode (ffmpeg genera una senal de prueba)")
        if mode == "direct":
            target = master_for(key) if q.get("master") == "1" else src
            return self._send(302, b"", "text/plain", {"Location": target or src})
        st, body = UP.get(src)
        if st in (401, 403, 404):
            src, info = pick_variant(key, max_h=max_h, force=True)
            if not src:
                return self._err(404, info)
            st, body = UP.get(src)
        if st != 200:
            return self._err(502, "lista de medios HTTP %s" % st)
        txt = rewrite_media(body.decode("utf-8", "replace"), src, self._host())
        return self._send(200, txt, "application/vnd.apple.mpegurl", {"X-TwitchWii-Variant": info})

    def _seg(self, u):
        p = urllib.parse.urlparse(u)
        if p.scheme != "https" or not SEG_HOSTS.search(p.hostname or ""):
            return self._err(403, "host no permitido")
        req = urllib.request.Request(u, headers={"User-Agent": UA})
        try:
            r = urllib.request.urlopen(req, timeout=20)
        except urllib.error.HTTPError as e:
            return self._err(e.code, "segmento HTTP %s" % e.code)
        except Exception as e:
            return self._err(502, "segmento: %s" % e)
        with r:
            data = r.read()
        self._send(200, data, r.headers.get("Content-Type", "video/mp4"))

    def _hls_file(self, key, name):
        d = TRANS.touch(key.lower()) if TRANS else None
        if not d:
            return self._err(404, "no hay transcodificacion activa para ese canal")
        fp = os.path.join(d, name)
        if not os.path.isfile(fp):
            return self._err(404, "no existe")
        with open(fp, "rb") as f:
            data = f.read()
        self._send(200, data, "application/vnd.apple.mpegurl" if name.endswith(".m3u8") else "video/mp2t")

    def _static(self, path):
        if path in ("", "/"):
            path = "/index.html"
        fp = os.path.normpath(os.path.join(PREVIEW_DIR, path.lstrip("/")))
        if not fp.startswith(PREVIEW_DIR) or not os.path.isfile(fp):
            return self._err(404, "no encontrado")
        ext = os.path.splitext(fp)[1].lower()
        ct = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
              ".js": "application/javascript; charset=utf-8", ".svg": "image/svg+xml",
              ".png": "image/png", ".json": "application/json"}.get(ext, "application/octet-stream")
        with open(fp, "rb") as f:
            self._send(200, f.read(), ct)


def lan_ips():
    ips = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ips.add(s.getsockname()[0])
        s.close()
    except Exception:
        pass
    return sorted(ips) or ["127.0.0.1"]


def main():
    global ARGS, TRANS, HELIX_API
    ap = argparse.ArgumentParser(description="TwitchWii companion server (no oficial)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8788)
    ap.add_argument("--lang", default="es", help="idioma por defecto (es, en, pt... o all)")
    ap.add_argument("--mock", action="store_true", help="forzar datos DEMO (sin tocar Twitch)")
    ap.add_argument("--play-mode", default="proxy", choices=["proxy", "direct", "transcode"])
    ap.add_argument("--tc-height", type=int, default=270)
    ap.add_argument("--tc-kbps", type=int, default=450)
    ap.add_argument("--gql-client-id", default=os.environ.get("TWITCH_GQL_CLIENT_ID", WEB_CLIENT_ID),
                    help="Client-ID para gql.twitch.tv (por defecto el de la web publica)")
    ap.add_argument("--oauth", default=os.environ.get("TWITCH_OAUTH", ""),
                    help="opcional: token OAuth de TU cuenta (cookie auth-token) para el token de reproduccion")
    ap.add_argument("--helix-id", default=os.environ.get("TWITCH_CLIENT_ID", ""), help="opcional: Client-ID de dev.twitch.tv (API Helix)")
    ap.add_argument("--helix-secret", default=os.environ.get("TWITCH_CLIENT_SECRET", ""), help="opcional: Client secret de dev.twitch.tv")
    ap.add_argument("--public-host", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ARGS = ap.parse_args()
    TRANS = Transcoder() if FFMPEG else None
    if ARGS.helix_id and ARGS.helix_secret:
        HELIX_API = Helix(ARGS.helix_id, ARGS.helix_secret)
    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), Handler)
    srv.daemon_threads = True
    log("TwitchWii companion %s escuchando en http://%s:%d" % (VERSION, ARGS.host, ARGS.port))
    for ip in lan_ips():
        log("  Wii / preview:  http://%s:%d/" % (ip, ARGS.port))
    log("  ffmpeg: %s | Pillow: %s | Helix: %s | OAuth: %s | mock: %s" % (
        FFMPEG or "NO (sin transcodificacion ni VODs)", "si" if Image else "NO (sin miniaturas)",
        "si" if HELIX_API else "no", "si" if ARGS.oauth else "no", ARGS.mock))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if TRANS:
            for s in list(TRANS.jobs):
                TRANS._kill(s)


if __name__ == "__main__":
    main()
