#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tv.h"
int cp_to_ascii(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    /* Latin-1 (tildes, enie, signos de apertura): se conserva como UTF-8; la fuente lo cubre */
    if ((cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) || cp == 0xA1 || cp == 0xBF) {
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    switch (cp) {
    case 0xE1: case 0xE0: case 0xE2: case 0xE4: case 0xE3: out[0]='a'; return 1;
    case 0xC1: case 0xC0: case 0xC2: case 0xC4: case 0xC3: out[0]='A'; return 1;
    case 0xE9: case 0xE8: case 0xEA: case 0xEB: out[0]='e'; return 1;
    case 0xC9: case 0xC8: case 0xCA: case 0xCB: out[0]='E'; return 1;
    case 0xED: case 0xEC: case 0xEE: case 0xEF: out[0]='i'; return 1;
    case 0xCD: case 0xCC: case 0xCE: case 0xCF: out[0]='I'; return 1;
    case 0xF3: case 0xF2: case 0xF4: case 0xF6: case 0xF5: out[0]='o'; return 1;
    case 0xD3: case 0xD2: case 0xD4: case 0xD6: case 0xD5: out[0]='O'; return 1;
    case 0xFA: case 0xF9: case 0xFB: case 0xFC: out[0]='u'; return 1;
    case 0xDA: case 0xD9: case 0xDB: case 0xDC: out[0]='U'; return 1;
    case 0xF1: out[0]='n'; out[1]='~'; return 2;
    case 0xD1: out[0]='N'; out[1]='~'; return 2;
    case 0xE7: out[0]='c'; return 1;
    case 0xC7: out[0]='C'; return 1;
    case 0xBF: out[0]='?'; return 1;
    case 0xA1: out[0]='!'; return 1;
    case 0xA0: out[0]=' '; return 1;
    case 0xAB: case 0xBB: case 0x201C: case 0x201D: out[0]='"'; return 1;
    case 0x2018: case 0x2019: out[0]='\''; return 1;
    case 0x2013: case 0x2014: out[0]='-'; return 1;
    case 0x2026: out[0]='.'; out[1]='.'; out[2]='.'; return 3;
    case 0xB7: case 0x2022: case 0x203A: out[0]='-'; return 1;
    default: return 0;
    }
}

static int decode_entity(const char *p, char *out, int *consumed) {
    /* p apunta a '&' */
    const char *semi = strchr(p, ';');
    if (!semi || semi - p > 10) { out[0] = '&'; *consumed = 1; return 1; }
    int n = (int)(semi - p) + 1;
    *consumed = n;
    if (p[1] == '#') {
        unsigned cp = (p[2] == 'x' || p[2] == 'X') ? strtoul(p + 3, NULL, 16) : strtoul(p + 2, NULL, 10);
        return cp_to_ascii(cp, out);
    }
    if (!strncmp(p, "&amp;", 5))  { out[0] = '&'; return 1; }
    if (!strncmp(p, "&lt;", 4))   { out[0] = '<'; return 1; }
    if (!strncmp(p, "&gt;", 4))   { out[0] = '>'; return 1; }
    if (!strncmp(p, "&quot;", 6)) { out[0] = '"'; return 1; }
    if (!strncmp(p, "&apos;", 6)) { out[0] = '\''; return 1; }
    if (!strncmp(p, "&nbsp;", 6)) { out[0] = ' '; return 1; }
    if (!strncmp(p, "&middot;", 8)) { out[0] = '-'; return 1; }
    if (!strncmp(p, "&raquo;", 7) || !strncmp(p, "&laquo;", 7)) { out[0] = '"'; return 1; }
    out[0] = ' ';
    return 1;
}

static int tag_is(const char *tag, const char *name) {
    int n = strlen(name);
    return !strncasecmp(tag, name, n) && (tag[n] == '>' || tag[n] == ' ' || tag[n] == '/' || tag[n] == '\t' || tag[n] == '\n' || tag[n] == '\r');
}

/* Quita etiquetas HTML de forma cruda; devuelve texto nuevo (malloc) */
char *strip_html(const char *html) {
    int len = strlen(html);
    char *out = malloc(len + 16);
    int o = 0;
    const char *p = html;
    int last_space = 1, nl_count = 2;
    if (!out) return NULL;

    /* Saltar a <body> si existe */
    const char *body = strcasestr(html, "<body");
    if (body) p = body;

    while (*p) {
        if (*p == '<') {
            const char *t = p + 1;
            int closing = 0;
            if (*t == '/') { closing = 1; t++; }
            if (!strncmp(p, "<!--", 4)) {
                const char *e = strstr(p + 4, "-->");
                p = e ? e + 3 : p + strlen(p);
                continue;
            }
            if (!closing && (tag_is(t, "script") || tag_is(t, "style") || tag_is(t, "noscript") || tag_is(t, "svg"))) {
                char endtag[16];
                int k = 0;
                while (k < 10 && isalpha((unsigned char)t[k])) { endtag[k] = t[k]; k++; }
                endtag[k] = 0;
                char pat[20];
                snprintf(pat, sizeof(pat), "</%s", endtag);
                const char *e = strcasestr(t, pat);
                if (!e) break;
                p = strchr(e, '>');
                p = p ? p + 1 : e + strlen(e);
                continue;
            }
            int block = tag_is(t, "br") || tag_is(t, "p") || tag_is(t, "div") || tag_is(t, "h1") ||
                        tag_is(t, "h2") || tag_is(t, "h3") || tag_is(t, "li") || tag_is(t, "tr") ||
                        tag_is(t, "table") || tag_is(t, "hr") || tag_is(t, "ul") || tag_is(t, "ol");
            int heading = !closing && tag_is(t, "h3");
            const char *e = strchr(p, '>');
            p = e ? e + 1 : p + strlen(p);
            if (block) {
                if (heading && nl_count < 2) { out[o++] = '\n'; nl_count++; }
                if (nl_count < 2 || (heading && nl_count < 3)) { out[o++] = '\n'; nl_count++; }
                last_space = 1;
                if (heading) { out[o++] = '*'; out[o++] = ' '; nl_count = 0; }
            } else if (!last_space) {
                /* separador suave entre etiquetas en linea */
            }
            continue;
        }
        char buf[4];
        int n = 0, consumed = 1;
        unsigned char ch = (unsigned char)*p;
        if (ch == '&') {
            n = decode_entity(p, buf, &consumed);
        } else if (ch >= 0x80) {
            unsigned cp = 0; int extra = 0;
            if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; extra = 1; }
            else if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; extra = 2; }
            else if ((ch & 0xF8) == 0xF0) { cp = ch & 0x07; extra = 3; }
            else { cp = ch; extra = 0; } /* latin-1 suelto */
            int k;
            for (k = 1; k <= extra && p[k]; k++) cp = (cp << 6) | (p[k] & 0x3F);
            consumed = k;
            n = cp_to_ascii(cp, buf);
        } else {
            buf[0] = (char)ch; n = 1;
        }
        p += consumed;
        int i;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
                if (!last_space) { out[o++] = ' '; last_space = 1; }
            } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
                out[o++] = c; last_space = 0; nl_count = 0;
            }
        }
    }
    out[o] = 0;
    return out;
}

