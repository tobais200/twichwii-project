/*
 * i18n.h - TwitchWii interface language support.
 * All user-facing strings are written in Spanish in the source (the original
 * language of the app) and wrapped in T("..."). T() looks the Spanish text up
 * in the generated table (i18n_table.c) and returns the translation for the
 * language currently selected. Spanish returns the text unchanged; an unknown
 * text also falls back to the Spanish original.
 */
#ifndef I18N_H
#define I18N_H
enum { LANG_EN = 0, LANG_ES, LANG_FR, LANG_DE, LANG_IT, LANG_NL, LANG_PT, LANG_JA, LANG_ZH, LANG_ZT, LANG_KO, LANG_COUNT };

extern int ui_lang;                          /* current interface language (LANG_*) */
const char *tr(const char *es);              /* translate a source (Spanish) string */
#define T(s) tr(s)
const char *lang_native_name(int lang);      /* "English", "Espa\xc3\xb1ol", ... in its own language */
const char *lang_code(int lang);             /* "en", "es", "fr", ... (value saved on SD/USB) */
int  lang_from_code(const char *code);       /* -1 if unknown */
int  lang_from_console(void);                /* CONF_GetLanguage(): any supported language, English otherwise */
int  lang_is_cjk(int lang);
void i18n_init(void);                        /* sort table, pick the console language */
void i18n_set(int lang);
/* Concatenate translated pieces into buf (NULL-terminated array of source strings). */
void tr_join(char *buf, int sz, const char *const *parts);
#endif
