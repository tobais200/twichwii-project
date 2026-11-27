/*
 * i18n.c - runtime for the interface language table (see i18n.h).
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <gccore.h>
#include "i18n.h"
#include "gfx.h"

typedef struct { const char *es; const char *t[LANG_COUNT]; } i18n_entry_t;
extern const i18n_entry_t i18n_table[];     /* i18n_table.c, sorted with strcmp on the Spanish text */
extern const int i18n_table_n;

int ui_lang = LANG_EN;

static const char *const names[LANG_COUNT] = {
    "English", "Espa\xc3\xb1ol", "Fran\xc3\xa7" "ais", "Deutsch", "Italiano", "Nederlands", "Portugu\xc3\xaas (BR)",
    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",                         /* Japanese */
    "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87",         /* Simplified Chinese */
    "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87",         /* Traditional Chinese */
    "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4"                         /* Korean */
};
static const char *const codes[LANG_COUNT] = { "en", "es", "fr", "de", "it", "nl", "pt", "ja", "zh-hans", "zh-hant", "ko" };

const char *lang_native_name(int l) { return (l >= 0 && l < LANG_COUNT) ? names[l] : names[0]; }
const char *lang_code(int l) { return (l >= 0 && l < LANG_COUNT) ? codes[l] : codes[0]; }
int lang_from_code(const char *c) {
    int i;
    if (!c) return -1;
    for (i = 0; i < LANG_COUNT; i++) if (!strcasecmp(c, codes[i])) return i;
    if (!strcasecmp(c, "pt-br") || !strcasecmp(c, "pt_br")) return LANG_PT;
    if (!strcasecmp(c, "zh") || !strcasecmp(c, "zh-cn") || !strcasecmp(c, "zh_cn")) return LANG_ZH;
    if (!strcasecmp(c, "zh-tw") || !strcasecmp(c, "zh_tw")) return LANG_ZT;
    return -1;
}

int lang_from_console(void) {
    CONF_Init();
    switch (CONF_GetLanguage()) {
    case CONF_LANG_SPANISH: return LANG_ES;
    case CONF_LANG_FRENCH:  return LANG_FR;
    case CONF_LANG_GERMAN:  return LANG_DE;
    case CONF_LANG_ITALIAN: return LANG_IT;
    case CONF_LANG_DUTCH:   return LANG_NL;
    case CONF_LANG_JAPANESE:     return LANG_JA;
    case CONF_LANG_SIMP_CHINESE: return LANG_ZH;
    case CONF_LANG_TRAD_CHINESE: return LANG_ZT;
    case CONF_LANG_KOREAN:       return LANG_KO;
    default:                return LANG_EN;   /* English, unknown */
    }
}

int lang_is_cjk(int l) { return l >= LANG_JA && l <= LANG_KO; }
void i18n_set(int l) {
    ui_lang = (l >= 0 && l < LANG_COUNT) ? l : LANG_EN;
    gfx_cjk_select(lang_is_cjk(ui_lang) ? ui_lang - LANG_JA + 1 : 0);   /* CJK glyphs of the active language only */
}
void i18n_init(void) { i18n_set(lang_from_console()); }

const char *tr(const char *es) {
    int lo = 0, hi = i18n_table_n - 1;
    if (!es || ui_lang == LANG_ES) return es;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1, c = strcmp(es, i18n_table[mid].es);
        if (!c) { const char *t = i18n_table[mid].t[ui_lang]; return (t && *t) ? t : es; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return es;
}

void tr_join(char *buf, int sz, const char *const *parts) {
    int n = 0;
    for (; *parts && n < sz - 1; parts++) {
        const char *s = tr(*parts);
        int l = (int)strlen(s);
        if (l > sz - 1 - n) l = sz - 1 - n;
        memcpy(buf + n, s, l); n += l;
    }
    buf[n] = 0;
}
