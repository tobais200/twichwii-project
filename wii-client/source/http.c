/*
 * http.c - HTTP/HTTPS (libcurl + mbedTLS) y JSON minimo (TwitchWii; adaptado de KickWii / Google for Wii)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <wiisocket.h>
#include <curl/curl.h>
#include "guard.h"

#include "cacert_pem.h"
#include "tv.h"

#define SH_MAX_RESP (2 * 1024 * 1024)

char *sh_strdup(const char *s) {
    char *d = strdup(s ? s : "");
    return d ? d : calloc(1, 1);
}

static size_t sh_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    sh_resp_t *r = (sh_resp_t *)ud;
    size_t n = size * nmemb, room = r->cap - r->len;
    if (n > room) n = room;
    if (r->len + n > r->alloc) {
        size_t na = r->alloc ? r->alloc * 2 : 64 * 1024;
        while (na < r->len + n) na *= 2;
        if (na > r->cap) na = r->cap;
        char *nd = realloc(r->data, na + 1);
        if (!nd) return size * nmemb;
        r->data = nd; r->alloc = na;
    }
    if (n) { memcpy(r->data + r->len, ptr, n); r->len += n; r->data[r->len] = 0; }
    return size * nmemb;
}

static int sh_progress_cb(void *c, curl_off_t a, curl_off_t b, curl_off_t d, curl_off_t e) {
    (void)c; (void)a; (void)b; (void)d; (void)e;
    ui_busy_tick();
    return (in_poll() & WPAD_BUTTON_HOME) ? 1 : 0;
}

int sh_net_ready(void) {
    int i, r;
    for (i = 0; i < 3; i++) {
        r = wiisocket_init();
        if (r == 0 || r == 1) return 1;
    }
    return 0;
}

int sh_request(const char *method, const char *url, const char *const *headers,
               const char *body, const char *ua, sh_resp_t *r) {
    struct curl_blob ca;
    struct curl_slist *hl = NULL;
    CURL *curl;
    char errbuf[CURL_ERROR_SIZE];
    int i;

    memset(r, 0, sizeof(*r));
    r->cap = SH_MAX_RESP;
    r->data = malloc(1);
    if (r->data) r->data[0] = 0;
    if (!sh_net_ready()) { r->res = CURLE_COULDNT_CONNECT; snprintf(r->err, sizeof(r->err), "La red no esta disponible."); return r->res; }

    curl = curl_easy_init();
    if (!curl) { r->res = CURLE_FAILED_INIT; snprintf(r->err, sizeof(r->err), "No se pudo iniciar libcurl."); return r->res; }
    errbuf[0] = 0;
    ca.data = (void *)cacert_pem; ca.len = cacert_pem_size; ca.flags = CURL_BLOB_NOCOPY;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua ? ua : TV_UA);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    sh_clock_attach(curl);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "HTTP,HTTPS");      /* HTTP: servidor companion en la LAN */
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "HTTP,HTTPS");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sh_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sh_progress_cb);
    hl = curl_slist_append(hl, "Accept: application/json");
    hl = curl_slist_append(hl, "Expect:");
    for (i = 0; headers && headers[i]; i++) hl = curl_slist_append(hl, headers[i]);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    if (method && strcmp(method, "GET")) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    }
    r->res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hl);
    if (r->res != CURLE_OK) {
        if (r->res == CURLE_ABORTED_BY_CALLBACK) snprintf(r->err, sizeof(r->err), "Cancelado (HOME).");
        else snprintf(r->err, sizeof(r->err), "curl %d: %s. %s", r->res, curl_easy_strerror(r->res), errbuf);
    }
    return r->res;
}

void sh_resp_free(sh_resp_t *r) { free(r->data); r->data = NULL; r->len = 0; }

int sh_ok(const sh_resp_t *r) { return r->res == CURLE_OK && r->code >= 200 && r->code < 300; }

void sh_error_text(const sh_resp_t *r, char *out, int outsz) {
    if (r->res != CURLE_OK) {
        snprintf(out, outsz, "Error de red: %s%s", r->err,
                 (r->res == CURLE_PEER_FAILED_VERIFICATION || r->res == CURLE_SSL_CONNECT_ERROR)
                 ? "\nSi es de certificado, revisa la fecha y hora de la Wii." : "");
        return;
    }
    char msg[300] = "";
    const char *d = r->data ? jws(r->data) : NULL;
    if (d && *d == '{') {
        const char *keys[] = { "error_description", "error", "message", "detail", "title", NULL };
        int i;
        for (i = 0; keys[i] && !msg[0]; i++) {
            const char *v = jget(d, keys[i]);
            if (v && *v == '"') jstr(v, msg, sizeof(msg));
            else if (v && *v == '{') { const char *m = jget(v, "message"); if (m) jstr(m, msg, sizeof(msg)); }
        }
    }
    if (!msg[0] && r->data) {
        char *t = sh_html_to_text(r->data);
        snprintf(msg, sizeof(msg), "%.280s", t ? t : "");
        free(t);
    }
    snprintf(out, outsz, "El servidor respondio HTTP %ld.\n%s", r->code, msg);
}

void sh_curl_common(void *c, char *errbuf) {
    struct curl_blob ca;
    CURL *curl = (CURL *)c;
    ca.data = (void *)cacert_pem; ca.len = cacert_pem_size; ca.flags = CURL_BLOB_NOCOPY;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, TV_UA);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    sh_clock_attach(curl);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "HTTP,HTTPS");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "HTTP,HTTPS");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    guard_curl(curl);   /* rc16: sockets registrados (el vigilante los puede cortar) */
}

char *sh_urlencode(const char *s) {
    char *e = curl_easy_escape(NULL, s, 0);
    char *d = sh_strdup(e ? e : "");
    if (e) curl_free(e);
    return d;
}

static void sh_json_escape_unused(const char *in, char *out, int outsz) {
    int o = 0;
    for (; *in && o < outsz - 7; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 32) o += snprintf(out + o, outsz - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = 0;
}

/* ---- configuracion en sd:/apps/google-wii/ (o usb:) ---- */
int sh_cfg_read(const char *name, char *buf, int sz) {
    const char *dirs[] = { "sd:/apps/TwitchWii/", "usb:/apps/TwitchWii/" };
    char path[160];
    int i;
    buf[0] = 0;
    for (i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char tmp[1024];
        size_t n = fread(tmp, 1, sizeof(tmp) - 1, f);
        fclose(f);
        tmp[n] = 0;
        char *s = tmp;
        while (*s && isspace((unsigned char)*s)) s++;
        char *e = s + strcspn(s, "\r\n");
        *e = 0;
        while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (*s) { snprintf(buf, sz, "%s", s); return 1; }
    }
    return 0;
}

int sh_cfg_write(const char *name, const char *value) {
    /* rc12: SD si esta montada, si no USB (antes solo sd:/). */
    char d[2][96], path[192];
    if (fs_data_dirs(TV_APP_NAME, d, 1) < 1) return -1;
    snprintf(path, sizeof(path), "%s%s", d[0], name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "%s\n", value);
    return fclose(f) == 0 ? 0 : -1;
}

char *sh_html_to_text(const char *html) {
    char *t = strip_html(html ? html : "");
    if (!t) return sh_strdup("");
    /* quitar lineas vacias al principio/final */
    char *s = t;
    while (*s == '\n' || *s == ' ') s++;
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == '\n' || s[l-1] == ' ')) s[--l] = 0;
    char *d = sh_strdup(s);
    free(t);
    return d;
}

/* ======================= JSON minimo ======================= */
const char *jws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *jskip_string(const char *p) {
    /* p apunta a '"' */
    p++;
    while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
    return *p ? p + 1 : p;
}

const char *jskip(const char *p) {
    p = jws(p);
    if (!p || !*p) return p;
    if (*p == '"') return jskip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = jskip_string(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

const char *jget(const char *obj, const char *key) {
    const char *p = jws(obj);
    size_t kl = strlen(key);
    if (!p || *p != '{') return NULL;
    p++;
    for (;;) {
        p = jws(p);
        if (!*p || *p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *ks = p + 1;
        const char *ke = jskip_string(p);       /* despues de la comilla final */
        int match = (size_t)(ke - 1 - ks) == kl && !strncmp(ks, key, kl);
        p = jws(ke);
        if (*p != ':') return NULL;
        p = jws(p + 1);
        if (match) return p;
        p = jws(jskip(p));
        if (*p == ',') p++;
    }
}

const char *jpath(const char *obj, const char *path) {
    char k[64];
    const char *v = obj;
    while (v && *path) {
        int n = strcspn(path, ".");
        if (n >= (int)sizeof(k)) return NULL;
        memcpy(k, path, n); k[n] = 0;
        v = jget(v, k);
        path += n;
        if (*path == '.') path++;
    }
    return v;
}

const char *jarr_first(const char *arr) {
    const char *p = jws(arr);
    if (!p || *p != '[') return NULL;
    p = jws(p + 1);
    return (*p && *p != ']') ? p : NULL;
}

const char *jarr_next(const char *elem) {
    const char *p = jws(jskip(elem));
    if (*p != ',') return NULL;
    p = jws(p + 1);
    return (*p && *p != ']') ? p : NULL;
}

int jis_null(const char *v) { v = jws(v); return !v || !strncmp(v, "null", 4); }

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0; int i;
    for (i = 0; i < 4; i++) {
        int c = p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + d;
    }
    *out = v;
    return 1;
}

/* decodifica a ASCII; si out==NULL solo cuenta */
static int jdecode(const char *v, char *out, int outsz) {
    int o = 0;
    char tmp[4];
    v = jws(v);
    if (!v || !*v || jis_null(v)) { if (out && outsz) out[0] = 0; return 0; }
    if (*v != '"') {
        const char *e = jskip(v);
        int l = (int)(e - v);
        if (out) { if (l >= outsz) l = outsz - 1; memcpy(out, v, l); out[l] = 0; }
        return l;
    }
    const char *p = v + 1;
    while (*p && *p != '"') {
        int n = 0;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': tmp[0] = '\n'; n = 1; p++; break;
            case 't': case 'r': tmp[0] = ' '; n = 1; p++; break;
            case 'b': case 'f': p++; break;
            case 'u': {
                unsigned cp;
                if (hex4(p + 1, &cp)) {
                    p += 5;
                    if(cp>=0xD800&&cp<=0xDBFF&&p[0]=='\\'&&p[1]=='u') {unsigned low;if(hex4(p+2,&low)&&low>=0xDC00&&low<=0xDFFF)p+=6;} /* bounded surrogate pair */
                    else if (cp < 0xD800 || cp > 0xDFFF) n = cp_to_ascii(cp, tmp);
                } else p++;
                break;
            }
            default: tmp[0] = *p; n = 1; p++; break;
            }
        } else if ((unsigned char)*p >= 0x80) {
            unsigned char ch = (unsigned char)*p;
            unsigned cp; int extra, k;
            if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; extra = 1; }
            else if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; extra = 2; }
            else if ((ch & 0xF8) == 0xF0) { cp = ch & 0x07; extra = 3; }
            else { cp = ch; extra = 0; }
            for (k = 1; k <= extra && p[k]; k++) cp = (cp << 6) | (p[k] & 0x3F);
            p += k;
            n = cp_to_ascii(cp, tmp);
        } else {
            tmp[0] = *p++; n = 1;
        }
        int i;
        for (i = 0; i < n; i++) {
            if (out) { if (o < outsz - 1) out[o] = tmp[i]; else break; }
            o++;
        }
    }
    if (out) out[o < outsz ? o : outsz - 1] = 0;
    return o;
}

int jstr(const char *v, char *out, int outsz) { return jdecode(v, out, outsz); }

char *jstr_dup(const char *v) {
    int n = jdecode(v, NULL, 0);
    char *d = malloc(n + 1);
    if (!d) return calloc(1, 1);
    jdecode(v, d, n + 1);
    return d;
}

long long jnum(const char *v) {
    v = jws(v);
    if (!v) return 0;
    if (*v == '"') v++;
    return strtoll(v, NULL, 10);
}

/* fuerza a enlazar inet_ntoa/inet_aton de libwiisocket (si no, libogc trae
 * soc_util.o via network_wii.o y hay simbolos duplicados con inet_ntop/pton) */
struct in_addr;
extern char *inet_ntoa(struct in_addr in);
extern int inet_aton(const char *cp, struct in_addr *inp);
void *ym_keep_inet[2] = { (void *)&inet_ntoa, (void *)&inet_aton };
