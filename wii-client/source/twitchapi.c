/*
 * twitchapi.c - TwitchWii: modelo de datos y los dos "backends":
 *   1) servidor companion en la LAN (recomendado): JSON compacto ya limpio (Latin-1),
 *      miniaturas RGB, video por HTTP plano (proxy 360p) o transcodificado (270p), VODs, chat
 *   2) directo a Twitch (sin PC): API GraphQL publica de la web (gql.twitch.tv) por HTTPS,
 *      token de reproduccion + usher.ttvnw.net. Sin miniaturas (la Wii no decodifica JPEG
 *      aca), sin VODs ni chat.
 * Solo lectura de datos publicos. Cada pedido lo dispara una accion del usuario.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "tv.h"
#include "auth.h"

#define GQL_URL  "https://gql.twitch.tv/gql"
#define GQL_CID  "kimne78kx3ncx6brgo4mv6wki5h1ko"     /* client-id publico de la web de twitch.tv */

char k_server[128] = "";
int  k_server_ok = 0;
int  k_play_mode = PM_PROXY;
int  k_lang = 0;
const char *k_langs[] = { "es", "all", "en", "pt" };
const char *k_lang_names[] = { "Espa\xc3\xb1ol", "Todos", "English", "Portugu\xc3\xaas" };
int  k_nlangs = sizeof(k_langs) / sizeof(k_langs[0]);
_Static_assert(sizeof(k_langs) == sizeof(k_lang_names), "language tables must match");
int  k_show_mature = 0;
int  k_chat_on = 1;
int  k_last_mock = 0;
char k_notice[200] = "";
const char *pm_names[PM_N] = { "Proxy 360p", "Transcodificado 270p", "Directo (HTTPS)" };

/* ------------------------------------------------------------------ listas */
void klist_clear(klist_t *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }
kstream_t *klist_add(klist_t *l) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        kstream_t *nv = realloc(l->v, nc * sizeof(kstream_t));
        if (!nv) return NULL;
        l->v = nv; l->cap = nc;
    }
    kstream_t *s = &l->v[l->n++];
    memset(s, 0, sizeof(*s));
    s->live = -1;
    return s;
}

void tcatlist_clear(tcatlist_t *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }
tcat_t *tcatlist_add(tcatlist_t *l) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        tcat_t *nv = realloc(l->v, (size_t)nc * sizeof(tcat_t));
        if (!nv) return NULL;
        l->v = nv; l->cap = nc;
    }
    tcat_t *c = &l->v[l->n++];
    memset(c, 0, sizeof(*c));
    return c;
}

int tw_is_gambling(const char *name, const char *slug) {
    static const char *bad[] = { "slot", "casino", "gambl", "poker", "roulette", "ruleta", "blackjack",
                                 "betting", "apuesta", "plinko", "crash", "dice", "baccarat", "sportsbook", NULL };
    char a[96]; int i, k;
    for (k = 0; k < 2; k++) {
        const char *s = k ? slug : name;
        if (!s) continue;
        snprintf(a, sizeof(a), "%s", s);
        for (i = 0; a[i]; i++) a[i] = tolower((unsigned char)a[i]);
        for (i = 0; bad[i]; i++) if (strstr(a, bad[i])) return 1;
    }
    return 0;
}

void kstream_style(kstream_t *s) {
    if (!k_server[0] && s->live == 1 && s->slug[0] && !s->thumb[0])
        snprintf(s->thumb, sizeof(s->thumb), "https://static-cdn.jtvnw.net/previews-ttv/live_user_%s-224x126.jpg", s->slug);
    /* colores de avatar por defecto, en la gama de Twitch */
    static const u32 pal[] = { 0x9146FF, 0x772CE8, 0x5C16C5, 0xEB0400, 0x00A3A3, 0xFF6905, 0x1E69FF, 0xFA1ED2, 0x00C853, 0x451093 };
    unsigned h = 5381; const char *p;
    for (p = s->slug; *p; p++) h = h * 33 + (unsigned char)*p;
    s->color = pal[h % 10];
    const char *n = s->name[0] ? s->name : s->slug;
    int o = 0;
    for (p = n; *p && o < 2; p++) if (isalnum((unsigned char)*p)) s->shortn[o++] = toupper((unsigned char)*p);
    s->shortn[o] = 0;
    if (!o) snprintf(s->shortn, sizeof(s->shortn), "?");
}

static int is_true(const char *v) { char b[8] = ""; if (!v) return 0; v = jws(v); if (*v == 't') return 1; jstr(v, b, sizeof(b)); return !strcmp(b, "true") || !strcmp(b, "1"); }
static void jcopy(const char *obj, const char *key, char *out, int sz) {
    const char *v = obj ? jget(obj, key) : NULL;
    out[0] = 0;
    if (v && !jis_null(v)) jstr(v, out, sz);
}
static int valid_login(const char *s) {
    int i;
    if (!s[0] || strlen(s) > 25) return 0;
    for (i = 0; s[i]; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    return 1;
}
static void lower(char *s) { for (; *s; s++) *s = tolower((unsigned char)*s); }

/* ------------------------------------------------------------------ ajustes en la SD */
void cfg_load(void) {
    char b[160];
    if (sh_cfg_read("aspecto.txt", b, sizeof(b))) gfx_set_aspect(!strcmp(b,"4:3")?1:!strcmp(b,"16:9")?2:0);
    if (sh_cfg_read("server.txt", b, sizeof(b))) {
        if (strncmp(b, "http://", 7) && strncmp(b, "https://", 8)) snprintf(k_server, sizeof(k_server), "http://%s", b);
        else snprintf(k_server, sizeof(k_server), "%s", b);
        int n = strlen(k_server);
        while (n > 0 && k_server[n - 1] == '/') k_server[--n] = 0;
        if (!strcmp(k_server, "http://") || !strcasecmp(b, "directo")) k_server[0] = 0;
    }
    if (sh_cfg_read("modo.txt", b, sizeof(b))) {
        if (!strcmp(b, "transcode")) k_play_mode = PM_TRANSCODE;
        else if (!strcmp(b, "direct")) k_play_mode = PM_DIRECT;
        else k_play_mode = PM_PROXY;
    }
    if (sh_cfg_read("idioma.txt", b, sizeof(b))) {
        int i;
        for (i = 0; i < k_nlangs; i++) if (!strcmp(b, k_langs[i])) k_lang = i;
    }
    if (sh_cfg_read("filtro.txt", b, sizeof(b))) k_show_mature = !strcmp(b, "mostrar-todo");
    if (sh_cfg_read("chat.txt", b, sizeof(b))) k_chat_on = strcmp(b, "no") != 0;
}
void cfg_save(void) {
    if (k_play_mode < 0 || k_play_mode >= PM_N) k_play_mode = PM_PROXY;
    sh_cfg_write("aspecto.txt", gfx_aspect_mode()==1?"4:3":gfx_aspect_mode()==2?"16:9":"auto");
    if (k_lang < 0 || k_lang >= k_nlangs) k_lang = 0;
    static const char *pm[PM_N] = { "proxy", "transcode", "direct" };
    sh_cfg_write("server.txt", k_server[0] ? k_server : "directo");
    sh_cfg_write("modo.txt", pm[k_play_mode]);
    sh_cfg_write("idioma.txt", k_langs[k_lang]);
    sh_cfg_write("filtro.txt", k_show_mature ? "mostrar-todo" : "ocultar-18-y-apuestas");
    sh_cfg_write("chat.txt", k_chat_on ? "si" : "no");
}

/* ------------------------------------------------------------------ HTTP */
static int check_resp(sh_resp_t *r, int srv, char *err, int esz) {
    if (r->res != 0) {
        if (srv) snprintf(err, esz, "No se pudo conectar al servidor %s (%s). Revis\xc3\xa1 la IP en Ajustes.", k_server, r->err);
        else snprintf(err, esz, "Error de red: %s (revis\xc3\xa1 Internet y la fecha/hora de la Wii)", r->err);
        return 0;
    }
    if (r->code == 404) {
        char m[160] = "";
        if (r->data && *jws(r->data) == '{') jcopy(jws(r->data), "error", m, sizeof(m));
        snprintf(err, esz, "%s", m[0] ? m : "No existe ese canal.");
        return 0;
    }
    if (r->code == 429) { snprintf(err, esz, "Demasiados pedidos: esper\xc3\xa1 unos segundos."); return 0; }
    if (!srv && (r->code == 403 || r->code == 401)) { snprintf(err, esz, "Twitch rechaz\xc3\xb3 el pedido (HTTP %ld). Us\xc3\xa1 el servidor companion.", r->code); return 0; }
    if (r->code < 200 || r->code >= 300) {
        char m[160] = "";
        if (r->data && *jws(r->data) == '{') jcopy(jws(r->data), "error", m, sizeof(m));
        snprintf(err, esz, "%s respondi\xc3\xb3 HTTP %ld. %s", srv ? "El servidor" : "Twitch", r->code, m);
        return 0;
    }
    if (!r->data || (*jws(r->data) != '{' && *jws(r->data) != '[')) { snprintf(err, esz, "Respuesta inesperada (no es JSON)."); return 0; }
    return 1;
}

static int kget(const char *url, sh_resp_t *r, char *err, int esz) {
    static const char *hdr[] = { "Accept-Language: es-AR,es;q=0.9", NULL };
    int srv = k_server[0] && !strncmp(url, k_server, strlen(k_server));
    sh_request("GET", url, hdr, NULL, NULL, r);
    return check_resp(r, srv, err, esz);
}

/* POST a gql.twitch.tv (modo directo). Devuelve el objeto "data" o NULL. */
static const char *gql(const char *body, sh_resp_t *r, char *err, int esz) {
    static const char *hdr[] = { "Client-Id: " GQL_CID, "Content-Type: text/plain;charset=UTF-8", NULL };
    sh_request("POST", GQL_URL, hdr, body, NULL, r);
    if (!check_resp(r, 0, err, esz)) return NULL;
    const char *d = jget(jws(r->data), "data");
    if (!d || jis_null(d)) {
        char m[160] = "";
        const char *e = jget(jws(r->data), "errors");
        if (e && (e = jarr_first(e))) jcopy(e, "message", m, sizeof(m));
        snprintf(err, esz, "Twitch (GraphQL): %s", m[0] ? m : "respuesta sin datos");
        return NULL;
    }
    return d;
}

/* escapa texto para meterlo en un string JSON */
static void jesc(const char *s, char *o, int osz) {
    int n = 0;
    for (; *s && n < osz - 7; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = c; }
        else if (c < 32) n += snprintf(o + n, osz - n, "\\u%04x", c);
        else o[n++] = c;
    }
    o[n] = 0;
}

int tw_health(char *err, int esz) {
    char url[200];
    sh_resp_t r;
    k_server_ok = 0;
    if (!k_server[0]) { snprintf(err, esz, "Sin servidor: modo directo a Twitch."); return 0; }
    snprintf(url, sizeof(url), "%s/wii/health", k_server);
    if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return 0; }
    k_server_ok = is_true(jget(jws(r.data), "ok"));
    if (!k_server_ok) snprintf(err, esz, "El servidor respondi\xc3\xb3 algo raro.");
    sh_resp_free(&r);
    return k_server_ok;
}

/* ------------------------------------------------------------------ parseo */
static void read_tags(const char *arr, kstream_t *s, int gql_form) {
    const char *t;
    s->ntags = 0;
    for (t = arr ? jarr_first(arr) : NULL; t && s->ntags < 3; t = jarr_next(t)) {
        if (gql_form) jcopy(t, "name", s->tags[s->ntags], sizeof(s->tags[0]));
        else jstr(t, s->tags[s->ntags], sizeof(s->tags[0]));
        if (s->tags[s->ntags][0]) s->ntags++;
    }
}

/* forma compacta del companion (/wii/live, /wii/channel, /wii/search) */
static void parse_compact(const char *e, kstream_t *s) {
    const char *v;
    jcopy(e, "login", s->slug, sizeof(s->slug));
    if (!s->slug[0]) jcopy(e, "slug", s->slug, sizeof(s->slug));
    jcopy(e, "name", s->name, sizeof(s->name));
    jcopy(e, "title", s->title, sizeof(s->title));
    jcopy(e, "category", s->cat, sizeof(s->cat));
    jcopy(e, "category_slug", s->catslug, sizeof(s->catslug));
    jcopy(e, "lang", s->lang, sizeof(s->lang));
    jcopy(e, "start_time", s->start, sizeof(s->start));
    jcopy(e, "thumb_rgb", s->thumb, sizeof(s->thumb));
    jcopy(e, "avatar_rgb", s->avatar, sizeof(s->avatar));
    jcopy(e, "banner_rgb", s->banner, sizeof(s->banner));
    jcopy(e, "bio", s->bio, sizeof(s->bio));
    if ((v = jget(e, "viewers"))) s->viewers = (int)jnum(v);
    if ((v = jget(e, "followers"))) s->followers = (int)jnum(v);
    s->verified = is_true(jget(e, "partner"));
    s->mature = is_true(jget(e, "mature"));
    s->gambling = is_true(jget(e, "gambling"));
    s->mock = is_true(jget(e, "mock"));
    read_tags(jget(e, "tags"), s, 0);
    v = jget(e, "live");
    s->live = v ? is_true(v) : 1;
    if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
    if (!k_server[0] && s->live == 1 && s->slug[0] && !s->thumb[0])
        snprintf(s->thumb, sizeof(s->thumb), "https://static-cdn.jtvnw.net/previews-ttv/live_user_%s-224x126.jpg", s->slug);
    kstream_style(s);
}

/* nodo "Stream" de GraphQL (modo directo) */
static void parse_gql_stream(const char *n, kstream_t *s) {
    const char *b = jget(n, "broadcaster"), *g = jget(n, "game"), *v;
    const char *bs = b ? jget(b, "broadcastSettings") : NULL;
    jcopy(b, "login", s->slug, sizeof(s->slug));
    jcopy(b, "displayName", s->name, sizeof(s->name));
    jcopy(n, "title", s->title, sizeof(s->title));
    if (g && !jis_null(g)) { jcopy(g, "displayName", s->cat, sizeof(s->cat)); jcopy(g, "slug", s->catslug, sizeof(s->catslug)); }
    jcopy(n, "createdAt", s->start, sizeof(s->start));
    jcopy(n, "previewImageURL", s->thumb, sizeof(s->thumb));
    jcopy(b, "profileImageURL", s->avatar, sizeof(s->avatar));
    if (bs && !jis_null(bs)) { jcopy(bs, "language", s->lang, sizeof(s->lang)); lower(s->lang); s->mature = is_true(jget(bs, "isMature")); }
    if ((v = jget(n, "viewersCount"))) s->viewers = (int)jnum(v);
    read_tags(jget(n, "freeformTags"), s, 1);
    s->gambling = tw_is_gambling(s->cat, s->catslug);
    s->live = 1;
    if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
    if (!k_server[0] && s->live == 1 && s->slug[0] && !s->thumb[0])
        snprintf(s->thumb, sizeof(s->thumb), "https://static-cdn.jtvnw.net/previews-ttv/live_user_%s-224x126.jpg", s->slug);
    kstream_style(s);
}

#define GQL_STREAM_FIELDS "title viewersCount createdAt previewImageURL(width:224,height:126) freeformTags{name} game{displayName slug} broadcaster{login displayName profileImageURL(width:70) broadcastSettings{isMature language}}"

/* ------------------------------------------------------------------ lista en vivo */
int tw_live(const char *game, char *after, int asz, klist_t *out, int *more, char *err, int esz) {
    static char url[600], body[1600];
    sh_resp_t r;
    const char *d, *arr, *e;
    int n = 0;
    *more = 0;
    k_notice[0] = 0;
    if (k_server[0]) {
        char *ea = sh_urlencode(after), *eg = sh_urlencode(game ? game : "");
        snprintf(url, sizeof(url), "%s/wii/live?limit=24&lang=%s&tw=224&th=126%s%s%s%s", k_server, k_langs[k_lang],
                 after[0] ? "&after=" : "", ea, (game && game[0]) ? "&game=" : "", eg);
        free(ea); free(eg);
        if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return -1; }
        d = jws(r.data);
        char src[16] = "";
        jcopy(d, "source", src, sizeof(src));
        k_last_mock = strncmp(src, "twitch", 6) != 0;
        jcopy(d, "notice", k_notice, sizeof(k_notice));
        jcopy(d, "next", after, asz);
        arr = jget(d, "items");
        for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
            kstream_t *s = klist_add(out);
            if (!s) break;
            parse_compact(e, s);
            if (!s->slug[0]) { out->n--; continue; }
            n++;
        }
    } else {
        char ca[200] = "null", lg[40] = "null", gs[64];
        k_last_mock = 0;
        if (after[0]) snprintf(ca, sizeof(ca), "\"%s\"", after);
        if (game && game[0]) {
            int byid = !strncmp(game, "id:", 3);
            jesc(byid ? game + 3 : game, gs, sizeof(gs));
            if (k_lang != 1) snprintf(lg, sizeof(lg), "[\"%s\"]", k_langs[k_lang]);
            if (byid)
                snprintf(body, sizeof(body),
                    "{\"query\":\"query($i:ID!,$n:Int,$a:Cursor,$l:[String!]){game(id:$i){streams(first:$n,after:$a,options:{languages:$l})"
                    "{edges{cursor node{" GQL_STREAM_FIELDS "}} pageInfo{hasNextPage}}}}\",\"variables\":{\"i\":\"%s\",\"n\":24,\"a\":%s,\"l\":%s}}",
                    gs, ca, lg);
            else
                snprintf(body, sizeof(body),
                    "{\"query\":\"query($s:String!,$n:Int,$a:Cursor,$l:[String!]){game(slug:$s){streams(first:$n,after:$a,options:{languages:$l})"
                    "{edges{cursor node{" GQL_STREAM_FIELDS "}} pageInfo{hasNextPage}}}}\",\"variables\":{\"s\":\"%s\",\"n\":24,\"a\":%s,\"l\":%s}}",
                    gs, ca, lg);
        } else {
            if (k_lang != 1) { snprintf(lg, sizeof(lg), "[\"%s\"]", k_langs[k_lang]); int i; for (i = 2; lg[i] && lg[i] != '"'; i++) lg[i] = toupper((unsigned char)lg[i]); }
            snprintf(body, sizeof(body),
                "{\"query\":\"query($n:Int,$a:Cursor,$l:[Language!]){streams(first:$n,after:$a,options:{languages:$l,sort:VIEWER_COUNT})"
                "{edges{cursor node{" GQL_STREAM_FIELDS "}} pageInfo{hasNextPage}}}\",\"variables\":{\"n\":24,\"a\":%s,\"l\":%s}}", ca, lg);
        }
        d = gql(body, &r, err, esz);
        if (!d) { sh_resp_free(&r); return -1; }
        const char *conn = (game && game[0]) ? jpath(d, "game.streams") : jget(d, "streams");
        arr = conn && !jis_null(conn) ? jget(conn, "edges") : NULL;
        after[0] = 0;
        for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
            const char *node = jget(e, "node");
            jcopy(e, "cursor", after, asz);
            if (!node || jis_null(node)) continue;
            kstream_t *s = klist_add(out);
            if (!s) break;
            parse_gql_stream(node, s);
            if (!s->slug[0]) { out->n--; continue; }
            n++;
        }
        if (conn && !is_true(jpath(conn, "pageInfo.hasNextPage"))) after[0] = 0;
    }
    *more = after[0] != 0 && n > 0;
    sh_resp_free(&r);
    return n;
}

/* ------------------------------------------------------------------ categorias (Explorar) */
static void style_cat(tcat_t *c) {
    unsigned h = 5381;
    const char *key = c->slug[0] ? c->slug : (c->id[0] ? c->id : c->name);
    const char *p;
    for (p = key; *p; p++) h = h * 33 + (unsigned char)*p;
    c->color = gfx_mix(0x9146FF, (h * 2654435761u) & 0xFFFFFF, 90);
    c->gambling = tw_is_gambling(c->name, c->slug);
}

static void helix_boxart(char *dst, int sz, const char *tmpl) {
    const char *s = tmpl ? tmpl : "";
    int n = 0;
    while (*s && n < sz - 1) {
        if (!strncmp(s, "{width}", 7)) { n += snprintf(dst + n, sz - n, "96"); s += 7; }
        else if (!strncmp(s, "{height}", 8)) { n += snprintf(dst + n, sz - n, "128"); s += 8; }
        else dst[n++] = *s++;
    }
    if (n < sz) dst[n] = 0;
}

static int cat_known(const tcatlist_t *l, const char *id, const char *slug) {
    int i;
    for (i = 0; i < l->n; i++) {
        if (id && id[0] && l->v[i].id[0] && !strcmp(l->v[i].id, id)) return 1;
        if (slug && slug[0] && l->v[i].slug[0] && !strcmp(l->v[i].slug, slug)) return 1;
    }
    return 0;
}

static void fill_cat_gql(tcat_t *c, const char *nd) {
    memset(c, 0, sizeof(*c));
    if (!nd) return;
    jcopy(nd, "id", c->id, sizeof(c->id));
    jcopy(nd, "displayName", c->name, sizeof(c->name));
    if (!c->name[0]) jcopy(nd, "name", c->name, sizeof(c->name));
    jcopy(nd, "slug", c->slug, sizeof(c->slug));
    jcopy(nd, "boxArtURL", c->boxart, sizeof(c->boxart));
    c->viewers = (int)jnum(jget(nd, "viewersCount"));
    style_cat(c);
}

static void fill_cat_helix(tcat_t *c, const char *nd) {
    char art[768];
    memset(c, 0, sizeof(*c));
    if (!nd) return;
    jcopy(nd, "id", c->id, sizeof(c->id));
    jcopy(nd, "name", c->name, sizeof(c->name));
    jcopy(nd, "box_art_url", art, sizeof(art));
    if (art[0]) helix_boxart(c->boxart, sizeof(c->boxart), art);
    style_cat(c);
}

/* rc11: Helix games/top y search/categories no traen espectadores (solo id/nombre/box art).
 * Se completa slug + viewersCount con el GQL publico, en tandas de 25 ids por consulta.
 * Sin esto cada categoria mostraba "0 espectadores" con la sesion iniciada. */
static void fill_missing_slugs(tcat_t *v, int from, int n, char *err, int esz) {
    char body[2400];
    int base;
    char e2[96];
    for (base = 0; base < n; base += 25) {
        int i, m = 0, pos, cnt = n - base < 25 ? n - base : 25;
        int idx[25];
        sh_resp_t r;
        const char *d;
        pos = snprintf(body, sizeof(body), "{\"query\":\"query{");
        for (i = 0; i < cnt && pos < (int)sizeof(body) - 90; i++) {
            tcat_t *c = &v[from + base + i];
            if (!c->id[0]) continue;
            pos += snprintf(body + pos, sizeof(body) - pos, "g%d:game(id:\\\"%s\\\"){id slug viewersCount} ", m, c->id);
            idx[m++] = from + base + i;
        }
        if (!m || pos >= (int)sizeof(body) - 8) continue;
        snprintf(body + pos, sizeof(body) - pos, "}\"}");
        memset(&r, 0, sizeof(r));
        d = gql(body, &r, e2, sizeof(e2));
        if (d) {
            int k;
            for (k = 0; k < m; k++) {
                char key[16], got[20] = "";
                const char *nd;
                tcat_t *c = &v[idx[k]];
                snprintf(key, sizeof(key), "g%d", k);
                nd = jget(d, key);
                if (!nd || jis_null(nd)) continue;
                jcopy(nd, "id", got, sizeof(got));
                if (strcmp(got, c->id)) continue;
                if (!c->slug[0]) jcopy(nd, "slug", c->slug, sizeof(c->slug));
                c->viewers = (int)jnum(jget(nd, "viewersCount"));
            }
        }
        sh_resp_free(&r);
    }
    (void)err; (void)esz;
}

#ifdef AUTOTEST_CATS_HELIX
#include "helix_canned.inc"
#endif
#define CAT_PAGE 100
#define CAT_MAX  2000
static int cats_started, cats_helix, cats_seed;
static char cats_cursor[256];
/* After the official top list, anonymous extra pages use search seeds:
 * GQL games(after:) requires a web integrity token the Wii cannot solve,
 * and Helix /games/top needs a user OAuth token (no app secret on a public client). */
static const char *cat_seeds[] = {
    "a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z",
    "0","1","2","3","4","5","6","7","8","9",
    "st","ma","po","the","el","la","de","game","war","super","re","co","an","sp","ch","li","pr","in","ar","sh",
    "mi","fo","ca","pa","ro","to","bo","ho","do","go","no","so","mo","lo","wo","fi","si","vi","pi","ki",
    "and","star","leg","call","mine","poke","fifa","nba","gta","zelda","mario","sonic","halo","final",
    "world","dark","dead","night","king","quest","hero","battle","craft","sim","race","fight","shoot",
    "jrpg","rpg","fps","mmo","sport","music","chess","card","party","horror","indie","retro","dragon",
    "soul","legend","metal","ninja","street","monster","counter","valorant","overwatch","roblox",
    NULL
};

void tw_categories_reset(void) {
    cats_started = 0;
    cats_helix = 0;
    cats_seed = 0;
    cats_cursor[0] = 0;
}

static int helix_games(const char *url, sh_resp_t *r, char *err, int esz) {
    const char *tok = auth_access_token();
    const char *cid = auth_client_id();
    char hcid[192], hauth[2200];
    const char *hdr[4];
    int n = 0;
    snprintf(hcid, sizeof(hcid), "Client-Id: %s", cid && cid[0] ? cid : "");
    hdr[n++] = hcid;
    if (tok && tok[0]) {
        snprintf(hauth, sizeof(hauth), "Authorization: Bearer %s", tok);
        hdr[n++] = hauth;
    }
    hdr[n++] = "Accept: application/json";
    hdr[n] = NULL;
    sh_request("GET", url, hdr, NULL, NULL, r);
    return check_resp(r, 0, err, esz);
}

static int append_helix_top(tcatlist_t *out, int *more, char *err, int esz) {
    char url[800];
    sh_resp_t r;
    const char *d, *arr, *e, *pg;
    int added = 0, from = out->n;
    char *ea = cats_cursor[0] ? sh_urlencode(cats_cursor) : NULL;
    snprintf(url, sizeof(url), "https://api.twitch.tv/helix/games/top?first=%d%s%s",
             CAT_PAGE, ea ? "&after=" : "", ea ? ea : "");
    free(ea);
    memset(&r, 0, sizeof(r));
#ifdef AUTOTEST_CATS_HELIX
    /* prueba: la respuesta de Helix /games/top (forma real, sin espectadores) sin token */
    if (cats_cursor[0]) { *more = 0; return 0; }
    r.data = sh_strdup(helix_canned); r.code = 200; r.len = strlen(r.data);
    if (0)
#endif
    if (!helix_games(url, &r, err, esz)) { sh_resp_free(&r); return -1; }
    d = jws(r.data);
    arr = jget(d, "data");
    cats_cursor[0] = 0;
    pg = jget(d, "pagination");
    if (pg) jcopy(pg, "cursor", cats_cursor, sizeof(cats_cursor));
    for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
        tcat_t tmp, *c;
        fill_cat_helix(&tmp, e);
        if (!tmp.id[0] || cat_known(out, tmp.id, tmp.slug) || out->n >= CAT_MAX) continue;
        c = tcatlist_add(out);
        if (!c) break;
        *c = tmp;
        added++;
    }
    if (added) fill_missing_slugs(out->v, from, added, err, esz);
    *more = cats_cursor[0] != 0 && added > 0 && out->n < CAT_MAX;
    sh_resp_free(&r);
    return added;
}

static int append_gql_top(tcatlist_t *out, int *more, char *err, int esz) {
    char body[400];
    sh_resp_t r;
    const char *d, *arr, *e;
    int added = 0;
    snprintf(body, sizeof(body),
        "{\"query\":\"query{games(first:%d){edges{node{id displayName slug viewersCount boxArtURL(width:96,height:128)}} pageInfo{hasNextPage}}}\"}",
        CAT_PAGE);
    d = gql(body, &r, err, esz);
    if (!d) { sh_resp_free(&r); return -1; }
    arr = jpath(d, "games.edges");
    for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
        const char *nd = jget(e, "node");
        tcat_t tmp, *c;
        fill_cat_gql(&tmp, nd);
        if ((!tmp.slug[0] && !tmp.id[0]) || cat_known(out, tmp.id, tmp.slug) || out->n >= CAT_MAX) continue;
        c = tcatlist_add(out);
        if (!c) break;
        *c = tmp;
        added++;
    }
    /* Official top is capped at 100 without integrity; extra pages use seeds. */
    *more = out->n < CAT_MAX && cat_seeds[0] != NULL;
    sh_resp_free(&r);
    return added;
}

static int append_search_seed(tcatlist_t *out, int *more, char *err, int esz) {
    char body[700], qe[40];
    sh_resp_t r;
    const char *d, *arr, *e;
    int added = 0, tries = 0;
    while (cat_seeds[cats_seed] && added == 0 && tries < 4 && out->n < CAT_MAX) {
        const char *q = cat_seeds[cats_seed++];
        jesc(q, qe, sizeof(qe));
        snprintf(body, sizeof(body),
            "{\"query\":\"query($q:String!,$n:Int){searchCategories(query:$q,first:$n)"
            "{edges{node{id displayName slug viewersCount boxArtURL(width:96,height:128)}}}}\","
            "\"variables\":{\"q\":\"%s\",\"n\":%d}}", qe, CAT_PAGE);
        d = gql(body, &r, err, esz);
        if (!d) { sh_resp_free(&r); if (added == 0 && tries == 0) return -1; break; }
        arr = jpath(d, "searchCategories.edges");
        for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
            const char *nd = jget(e, "node");
            tcat_t tmp, *c;
            fill_cat_gql(&tmp, nd);
            if ((!tmp.slug[0] && !tmp.id[0]) || cat_known(out, tmp.id, tmp.slug) || out->n >= CAT_MAX) continue;
            c = tcatlist_add(out);
            if (!c) break;
            *c = tmp;
            added++;
        }
        sh_resp_free(&r);
        tries++;
    }
    *more = cat_seeds[cats_seed] != NULL && out->n < CAT_MAX;
    return added;
}

int tw_categories_page(tcatlist_t *out, int *more, char *err, int esz) {
    int n;
    *more = 0;
    if (!out) return -1;
    if (out->n >= CAT_MAX) return 0;
    if (!cats_started) {
        cats_started = 1;
        cats_cursor[0] = 0;
        cats_seed = 0;
        cats_helix = 0;
#ifdef AUTOTEST_CATS_HELIX
        if (1) {
#else
        if (auth_ready() && auth_access_token()[0]) {
#endif
            n = append_helix_top(out, more, err, esz);
            if (n >= 0) { cats_helix = 1; return n; }
            /* Fall back to anonymous GQL if Helix is rejected. */
            err[0] = 0;
        }
        return append_gql_top(out, more, err, esz);
    }
    if (cats_helix) return append_helix_top(out, more, err, esz);
    return append_search_seed(out, more, err, esz);
}

int tw_search_categories(const char *q, tcatlist_t *out, char *err, int esz) {
    char body[700], qe[80], url[800];
    sh_resp_t r;
    const char *d, *arr, *e;
    int n = 0;
    if (!q || !q[0] || !out) return -1;
    tcatlist_clear(out);
    if (auth_ready() && auth_access_token()[0]) {
        char *eq = sh_urlencode(q);
        snprintf(url, sizeof(url), "https://api.twitch.tv/helix/search/categories?query=%s&first=%d", eq, CAT_PAGE);
        free(eq);
        memset(&r, 0, sizeof(r));
        if (helix_games(url, &r, err, esz)) {
            int from = 0;
            arr = jget(jws(r.data), "data");
            for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
                tcat_t tmp, *c;
                fill_cat_helix(&tmp, e);
                if (!tmp.id[0]) continue;
                c = tcatlist_add(out);
                if (!c) break;
                *c = tmp;
                n++;
            }
            if (n) fill_missing_slugs(out->v, from, n, err, esz);
            sh_resp_free(&r);
            if (n) return n;
        } else {
            sh_resp_free(&r);
        }
        err[0] = 0;
    }
    jesc(q, qe, sizeof(qe));
    snprintf(body, sizeof(body),
        "{\"query\":\"query($q:String!,$n:Int){searchCategories(query:$q,first:$n)"
        "{edges{node{id displayName slug viewersCount boxArtURL(width:96,height:128)}}}}\","
        "\"variables\":{\"q\":\"%s\",\"n\":%d}}", qe, CAT_PAGE);
    d = gql(body, &r, err, esz);
    if (!d) { sh_resp_free(&r); return -1; }
    arr = jpath(d, "searchCategories.edges");
    for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
        const char *nd = jget(e, "node");
        tcat_t tmp, *c;
        fill_cat_gql(&tmp, nd);
        if (!tmp.slug[0] && !tmp.id[0]) continue;
        c = tcatlist_add(out);
        if (!c) break;
        *c = tmp;
        n++;
    }
    sh_resp_free(&r);
    return n;
}

int tw_top_categories(tcat_t *out, int max, char *err, int esz) {
    tcatlist_t tmp = {0};
    int more = 0, i, n, saved_started, saved_helix, saved_seed;
    char saved_cursor[256];
    if (!out || max <= 0) return -1;
    saved_started = cats_started;
    saved_helix = cats_helix;
    saved_seed = cats_seed;
    snprintf(saved_cursor, sizeof(saved_cursor), "%s", cats_cursor);
    /* Isolated first-page fetch; do not disturb Explorar pagination. */
    tw_categories_reset();
    n = append_gql_top(&tmp, &more, err, esz);
    cats_started = saved_started;
    cats_helix = saved_helix;
    cats_seed = saved_seed;
    snprintf(cats_cursor, sizeof(cats_cursor), "%s", saved_cursor);
    if (n < 0) { tcatlist_clear(&tmp); return -1; }
    if (n > max) n = max;
    for (i = 0; i < n; i++) out[i] = tmp.v[i];
    tcatlist_clear(&tmp);
    return n;
}

/* ------------------------------------------------------------------ busqueda */
int tw_search(const char *q, klist_t *out, char *err, int esz) {
    static char url[300], body[900];
    sh_resp_t r;
    const char *d, *arr, *e;
    int n = 0;
    if (k_server[0]) {
        char *eq = sh_urlencode(q);
        snprintf(url, sizeof(url), "%s/wii/search?q=%s", k_server, eq);
        free(eq);
        if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return -1; }
        arr = jget(jws(r.data), "channels");
        for (e = arr ? jarr_first(arr) : NULL; e && n < 30; e = jarr_next(e)) {
            kstream_t *s = klist_add(out);
            if (!s) break;
            parse_compact(e, s);
            if (!s->slug[0]) { out->n--; continue; }
            n++;
        }
    } else {
        char qe[100];
        jesc(q, qe, sizeof(qe));
        snprintf(body, sizeof(body),
            "{\"query\":\"query($q:String!){searchFor(userQuery:$q,platform:\\\"web\\\"){channels{items{login displayName profileImageURL(width:70) followers{totalCount} roles{isPartner} "
            "stream{viewersCount previewImageURL(width:224,height:126) game{displayName}} broadcastSettings{title}}}}}\",\"variables\":{\"q\":\"%s\"}}", qe);
        d = gql(body, &r, err, esz);
        if (!d) { sh_resp_free(&r); return -1; }
        arr = jpath(d, "searchFor.channels.items");
        for (e = arr ? jarr_first(arr) : NULL; e && n < 30; e = jarr_next(e)) {
            kstream_t *s = klist_add(out);
            const char *st = jget(e, "stream");
            if (!s) break;
            jcopy(e, "login", s->slug, sizeof(s->slug));
            jcopy(e, "displayName", s->name, sizeof(s->name));
            jcopy(e, "profileImageURL", s->avatar, sizeof(s->avatar));
            s->followers = (int)jnum(jpath(e, "followers.totalCount"));
            s->verified = is_true(jpath(e, "roles.isPartner"));
            jcopy(jget(e, "broadcastSettings"), "title", s->title, sizeof(s->title));
            s->live = st && !jis_null(st);
            if (s->live) { jcopy(st, "previewImageURL", s->thumb, sizeof(s->thumb)); s->viewers = (int)jnum(jget(st, "viewersCount")); jcopy(jget(st, "game"), "displayName", s->cat, sizeof(s->cat)); }
            if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
            kstream_style(s);
            if (!s->slug[0]) { out->n--; continue; }
            n++;
        }
    }
    /* en vivo primero */
    int i, j;
    for (i = 1; i < out->n; i++) for (j = i; j > 0 && out->v[j].live > out->v[j - 1].live; j--) {
        kstream_t t = out->v[j]; out->v[j] = out->v[j - 1]; out->v[j - 1] = t;
    }
    sh_resp_free(&r);
    return n;
}

/* ------------------------------------------------------------------ canal */
static void parse_vods(const char *arr, int gql_form, tvod_t *vods, int maxv, int *nv) {
    const char *e;
    *nv = 0;
    for (e = arr ? jarr_first(arr) : NULL; e && *nv < maxv; e = jarr_next(e)) {
        const char *x = gql_form ? jget(e, "node") : e;
        tvod_t *v = &vods[*nv];
        if (!x || jis_null(x)) continue;
        memset(v, 0, sizeof(*v));
        jcopy(x, "id", v->id, sizeof(v->id));
        jcopy(x, "title", v->title, sizeof(v->title));
        if (gql_form) {
            jcopy(x, "createdAt", v->created, sizeof(v->created));
            jcopy(x, "previewThumbnailURL", v->thumb, sizeof(v->thumb));
            v->length = (int)jnum(jget(x, "lengthSeconds"));
            v->views = (int)jnum(jget(x, "viewCount"));
            jcopy(jget(x, "game"), "displayName", v->cat, sizeof(v->cat));
        } else {
            jcopy(x, "created", v->created, sizeof(v->created));
            jcopy(x, "thumb_rgb", v->thumb, sizeof(v->thumb));
            jcopy(x, "category", v->cat, sizeof(v->cat));
            v->length = (int)jnum(jget(x, "length"));
            v->views = (int)jnum(jget(x, "views"));
        }
        /* Twitch returns its own 404_processing graphic for VODs not ready yet. */
        if(gql_form && (!v->thumb[0] || strstr(v->thumb,"/_404/")))continue;
        if (v->id[0]) (*nv)++;
    }
}

int tw_channel(const char *login_in, kstream_t *s, tvod_t *vods, int maxv, int *nv, char *err, int esz) {
    static char url[300], body[1200];
    char login[48];
    sh_resp_t r;
    int dummy;
    if (!nv) nv = &dummy;
    *nv = 0;
    snprintf(login, sizeof(login), "%s", login_in);
    lower(login);
    int demo = !strncmp(login, "demo_", 5);
    if (!valid_login(login) && !demo) { snprintf(err, esz, "Nombre de canal inv\xc3\xa1lido: \"%s\".", login_in); return -1; }
    kstream_t keep = *s;
    if (k_server[0]) {
        snprintf(url, sizeof(url), "%s/wii/channel/%s", k_server, login);
        if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return -1; }
        const char *d = jws(r.data);
        memset(s, 0, sizeof(*s));
        parse_compact(d, s);
        s->live = is_true(jget(d, "live"));
        if (vods) parse_vods(jget(d, "videos"), 0, vods, maxv, nv);
    } else {
        snprintf(body, sizeof(body),
            "{\"query\":\"query($l:String!){user(login:$l){login displayName description profileImageURL(width:70) bannerImageURL offlineImageURL followers{totalCount} roles{isPartner} "
            "broadcastSettings{title language isMature game{displayName slug}} stream{type title viewersCount createdAt previewImageURL(width:224,height:126) freeformTags{name} game{displayName slug}} "
            "videos(first:6,type:ARCHIVE,sort:TIME){edges{node{id title lengthSeconds createdAt viewCount previewThumbnailURL(width:224,height:126) game{displayName}}}}}}\",\"variables\":{\"l\":\"%s\"}}", login);
        const char *d = gql(body, &r, err, esz);
        if (!d) { sh_resp_free(&r); return -1; }
        const char *u = jget(d, "user");
        if (!u || jis_null(u)) { snprintf(err, esz, "No existe ese canal."); sh_resp_free(&r); return -1; }
        const char *st = jget(u, "stream"), *bs = jget(u, "broadcastSettings");
        memset(s, 0, sizeof(*s));
        jcopy(u, "login", s->slug, sizeof(s->slug));
        jcopy(u, "displayName", s->name, sizeof(s->name));
        jcopy(u, "description", s->bio, sizeof(s->bio));
        jcopy(u, "profileImageURL", s->avatar, sizeof(s->avatar));
        jcopy(u, "bannerImageURL", s->banner, sizeof(s->banner));
        jcopy(u, "offlineImageURL", s->thumb, sizeof(s->thumb));
        if (!s->thumb[0]) snprintf(s->thumb, sizeof(s->thumb), "%s", s->banner);
        s->followers = (int)jnum(jpath(u, "followers.totalCount"));
        s->verified = is_true(jpath(u, "roles.isPartner"));
        if (bs && !jis_null(bs)) {
            jcopy(bs, "title", s->title, sizeof(s->title));
            jcopy(bs, "language", s->lang, sizeof(s->lang)); lower(s->lang);
            s->mature = is_true(jget(bs, "isMature"));
            const char *g = jget(bs, "game");
            if (g && !jis_null(g)) jcopy(g, "displayName", s->cat, sizeof(s->cat));
        }
        s->live = 0;
        if (st && !jis_null(st)) {
            char ty[16]; jcopy(st, "type", ty, sizeof(ty));
            s->live = !strcmp(ty, "live");
            jcopy(st, "title", s->title, sizeof(s->title));
            jcopy(st, "previewImageURL", s->thumb, sizeof(s->thumb));
            jcopy(st, "createdAt", s->start, sizeof(s->start));
            s->viewers = (int)jnum(jget(st, "viewersCount"));
            const char *g = jget(st, "game");
            if (g && !jis_null(g)) { jcopy(g, "displayName", s->cat, sizeof(s->cat)); jcopy(g, "slug", s->catslug, sizeof(s->catslug)); }
            read_tags(jget(st, "freeformTags"), s, 1);
        }
        s->gambling = tw_is_gambling(s->cat, s->catslug);
        if (vods) parse_vods(jpath(u, "videos.edges"), 1, vods, maxv, nv);
        if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
    }
    if (!s->slug[0]) snprintf(s->slug, sizeof(s->slug), "%s", login);
    /* sin miniatura propia: conservar la de la tarjeta */
    if (!s->thumb[0] && !strcmp(keep.slug, s->slug)) snprintf(s->thumb, sizeof(s->thumb), "%s", keep.thumb);
    if (!s->avatar[0] && !strcmp(keep.slug, s->slug)) snprintf(s->avatar, sizeof(s->avatar), "%s", keep.avatar);
    kstream_style(s);
    sh_resp_free(&r);
    return s->live;
}

/* ------------------------------------------------------------------ video */
/* respuesta de /wii/token/<login> (o cualquier JSON con "playback_url") */
int tw_parse_token(const char *json, kstream_t *info, char *url, int usz) {
    const char *d = jws(json);
    (void)info;
    url[0] = 0;
    if (!d || *d != '{') return -1;
    jcopy(d, "playback_url", url, usz);
    return url[0] ? 1 : 0;
}

static int direct_usher(const char *login, char *url, int usz, char *err, int esz) {
    static char body[600];
    sh_resp_t r;
    char tok[2048], sig[80];
    snprintf(body, sizeof(body),
        "{\"query\":\"query{streamPlaybackAccessToken(channelName:\\\"%s\\\",params:{platform:\\\"web\\\",playerBackend:\\\"mediaplayer\\\",playerType:\\\"site\\\"}){value signature}}\"}", login);
    const char *d = gql(body, &r, err, esz);
    if (!d) { sh_resp_free(&r); return 0; }
    const char *t = jget(d, "streamPlaybackAccessToken");
    if (!t || jis_null(t)) { snprintf(err, esz, "Twitch no dio acceso al video de %s.", login); sh_resp_free(&r); return 0; }
    jcopy(t, "value", tok, sizeof(tok));
    jcopy(t, "signature", sig, sizeof(sig));
    sh_resp_free(&r);
    char *et = sh_urlencode(tok);
    snprintf(url, usz, "https://usher.ttvnw.net/api/channel/hls/%s.m3u8?sig=%s&token=%s&allow_source=true&fast_bread=true"
             "&p=%d&player_backend=mediaplayer&playlist_include_framerate=true&supported_codecs=h264", login, sig, et, rand() % 9000000 + 100000);
    free(et);
    return 1;
}

int tw_play_url(const kstream_t *s, char *url, int usz, char *refresh, int rsz, int *max_h, char *err, int esz) {
    static const char *pm[PM_N] = { "proxy", "transcode", "direct" };
    refresh[0] = 0;
    *max_h = 360;
    if (!s->live) { snprintf(err, esz, "%s no est\xc3\xa1 transmitiendo ahora.", s->name); return 0; }
    if (!k_show_mature && (s->mature || s->gambling)) {
        snprintf(err, esz, "Oculto por el filtro: %s. Se cambia en Ajustes.", s->gambling ? "categor\xc3\xad" "a de apuestas/casino" : "contenido +18");
        return 0;
    }
    if (k_server[0]) {
        snprintf(url, usz, "%s/play/%s.m3u8?mode=%s&max_h=360", k_server, s->slug, pm[k_play_mode]);
        /* en modo directo el token de usher vence: se renueva pidiendolo al companion */
        if (k_play_mode == PM_DIRECT) snprintf(refresh, rsz, "%s/wii/token/%s", k_server, s->slug);
        return 1;
    }
    return direct_usher(s->slug, url, usz, err, esz);
}

int tw_vod_url(const tvod_t *v,char *url,int usz,char *err,int esz) {
    if(k_server[0]) {snprintf(url,usz,"%s/play/vod/%s.m3u8?mode=transcode",k_server,v->id);return 1;}
    int i;for(i=0;v->id[i];i++)if(v->id[i]<'0'||v->id[i]>'9'){snprintf(err,esz,"ID de VOD invalido");return 0;}
    if(!i){snprintf(err,esz,"ID de VOD vacio");return 0;}
    char body[900];sh_resp_t r;
    snprintf(body,sizeof(body),"{\"query\":\"query PlaybackAccessToken($vodID:ID!,$isVod:Boolean!){videoPlaybackAccessToken(id:$vodID,params:{platform:\\\"web\\\",playerBackend:\\\"mediaplayer\\\",playerType:\\\"site\\\"}) @include(if:$isVod){value signature}}\",\"variables\":{\"vodID\":\"%s\",\"isVod\":true}}",v->id);
    const char *d=gql(body,&r,err,esz);if(!d){sh_resp_free(&r);return 0;}
    const char *t=jget(d,"videoPlaybackAccessToken");char tok[4096],sig[128];
    if(!t||jis_null(t)){snprintf(err,esz,"VOD restringido, eliminado o solo para suscriptores.");sh_resp_free(&r);return 0;}
    jcopy(t,"value",tok,sizeof(tok));jcopy(t,"signature",sig,sizeof(sig));sh_resp_free(&r);
    if(!tok[0]||!sig[0]||is_true(jpath(tok,"authorization.forbidden"))){snprintf(err,esz,"Twitch no autoriza este VOD sin iniciar sesion/suscripcion.");return 0;}
    char *et=sh_urlencode(tok);
    int n=snprintf(url,usz,"https://usher.ttvnw.net/vod/%s.m3u8?nauthsig=%s&nauth=%s&allow_source=true&allow_audio_only=true&player=twitchweb&supported_codecs=h264",v->id,sig,et);
    free(et);memset(tok,0,sizeof(tok));
    if(n>=usz){snprintf(err,esz,"URL VOD demasiado larga");return 0;}return 1;
}
