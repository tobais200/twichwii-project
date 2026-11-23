/* memtrace.c: solo con -DMEMTRACE (diagnostico de memoria; nunca en un release).
 * Envuelve _malloc_r/_free_r/_realloc_r/_calloc_r/_memalign_r (-Wl,--wrap=...) y lleva la cuenta
 * de cada bloque vivo por sitio de llamada (5 direcciones de retorno). La tabla vive en MEM2,
 * por encima del heap (baja Arena2Hi 2 MB). memtrace_dump() escribe los sitios con mas bytes vivos. */
#ifdef MEMTRACE
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <reent.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
void *__real__malloc_r(struct _reent *, size_t);
void  __real__free_r(struct _reent *, void *);
void *__real__realloc_r(struct _reent *, void *, size_t);
void *__real__calloc_r(struct _reent *, size_t, size_t);
void *__real__memalign_r(struct _reent *, size_t, size_t);
#define NP 131072u
#define NS 16384u
#define DEPTH 8
typedef struct { u32 p, sz; u16 site; u16 pad; } ent_t;
typedef struct { u32 lr[DEPTH]; u32 bytes, cnt, prev_bytes, allocs; } site_t;
static ent_t *tab; static site_t *sites; static int ready, failed;
static u32 nsites, live_bytes, live_cnt, dropped;
void memtrace_start(void) {
    if (ready || failed) return;
    u32 need = NP * sizeof(ent_t) + NS * sizeof(site_t);
    u32 hi = (u32)SYS_GetArena2Hi(), lo = (u32)SYS_GetArena2Lo();
    u32 base = (hi - need) & ~31u;
    if (!hi || base <= lo + 4 * 1024 * 1024) { failed = 1; return; }
    SYS_SetArena2Hi((void *)base);
    memset((void *)base, 0, hi - base);
    tab = (ent_t *)base; sites = (site_t *)(base + NP * sizeof(ent_t)); nsites = 1; ready = 1;
}
static inline int okfp(u32 a) { return !(a & 3) && ((a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u)); }
static u16 site_now(void) {
    u32 lr[DEPTH] = { 0 }; u32 *fp; __asm__ volatile("mr %0,1" : "=r"(fp));
    for (int d = 0; d < DEPTH + 1; d++) {
        if (!okfp((u32)fp)) break; fp = (u32 *)fp[0]; if (!okfp((u32)fp)) break;
        if (d) lr[d - 1] = fp[1];   /* d=0: retorno dentro del propio wrapper */
    }
    u32 h = 2166136261u; for (int i = 0; i < DEPTH; i++) h = (h ^ lr[i]) * 16777619u;
    for (u32 k = 0; k < NS; k++) {
        u32 i = (h + k) % NS; if (!i) continue;
        if (!sites[i].allocs) { memcpy(sites[i].lr, lr, sizeof lr); if (i >= nsites) nsites = i + 1; return i; }
        if (!memcmp(sites[i].lr, lr, sizeof lr)) return i;
    }
    return 0;
}
static inline u32 hp(u32 p) { return ((p >> 3) * 2654435761u) & (NP - 1); }
static void add(void *ptr, size_t sz) {
    if (!ptr) return; if (!ready) return; u32 l; _CPU_ISR_Disable(l);
    {
        u16 s = site_now(); u32 p = (u32)ptr, i = hp(p), n = 0;
        while (tab[i].p && tab[i].p != p && n < NP) { i = (i + 1) & (NP - 1); n++; }
        if (n >= NP) dropped++;
        else {
            if (tab[i].p == p) { sites[tab[i].site].bytes -= tab[i].sz; sites[tab[i].site].cnt--; live_bytes -= tab[i].sz; live_cnt--; }
            tab[i].p = p; tab[i].sz = sz; tab[i].site = s; sites[s].bytes += sz; sites[s].cnt++; sites[s].allocs++; live_bytes += sz; live_cnt++;
        }
    }
    _CPU_ISR_Restore(l);
}
static void del(void *ptr) {
    if (!ptr || !ready) return; u32 l; _CPU_ISR_Disable(l);
    u32 p = (u32)ptr, i = hp(p), n = 0;
    while (tab[i].p && tab[i].p != p && n < NP) { i = (i + 1) & (NP - 1); n++; }
    if (tab[i].p == p) {
        sites[tab[i].site].bytes -= tab[i].sz; sites[tab[i].site].cnt--; live_bytes -= tab[i].sz; live_cnt--;
        tab[i].p = 0;   /* borrado con corrimiento hacia atras (sondeo lineal) */
        u32 j = i;
        for (;;) {
            j = (j + 1) & (NP - 1); if (!tab[j].p) break;
            u32 k = hp(tab[j].p);
            if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) { tab[i] = tab[j]; tab[j].p = 0; i = j; }
        }
    }
    _CPU_ISR_Restore(l);
}
void *__wrap__malloc_r(struct _reent *r, size_t n) { void *p = __real__malloc_r(r, n); add(p, n); return p; }
void  __wrap__free_r(struct _reent *r, void *p) { del(p); __real__free_r(r, p); }
void *__wrap__calloc_r(struct _reent *r, size_t a, size_t b) { void *p = __real__calloc_r(r, a, b); add(p, a * b); return p; }
void *__wrap__memalign_r(struct _reent *r, size_t al, size_t n) { void *p = __real__memalign_r(r, al, n); add(p, n); return p; }
void *__wrap__realloc_r(struct _reent *r, void *o, size_t n) {
    void *p = __real__realloc_r(r, o, n);
    if (p || !n) { if (o && o != p) del(o); if (p) add(p, n); }
    return p;
}
void memtrace_dump(FILE *f, const char *tag) {
    if (!f || !ready) return;
    static site_t snap[48]; static u16 ids[48]; int ns = 0;
    u32 l; _CPU_ISR_Disable(l);
    for (u32 i = 1; i < nsites; i++) {   /* top 48 por bytes vivos */
        if (!sites[i].bytes) continue;
        int k = ns < 48 ? ns++ : 47;
        if (k == 47 && ns == 48 && sites[i].bytes <= snap[47].bytes) continue;
        snap[k] = sites[i]; ids[k] = i;
        while (k > 0 && snap[k].bytes > snap[k - 1].bytes) { site_t t = snap[k]; snap[k] = snap[k - 1]; snap[k - 1] = t; u16 x = ids[k]; ids[k] = ids[k - 1]; ids[k - 1] = x; k--; }
    }
    for (int k = 0; k < ns; k++) sites[ids[k]].prev_bytes = sites[ids[k]].bytes;
    u32 lb = live_bytes, lc = live_cnt, nsi = nsites, dr = dropped;
    _CPU_ISR_Restore(l);
    struct mallinfo mi = mallinfo();
    fprintf(f, "memtrace %s: live=%uKB blocks=%u sites=%u dropped=%u | arena=%dKB inuse=%dKB free=%dKB freechunks=%d | mem2_free=%uKB\n", tag, lb / 1024, lc, nsi, dr,
            mi.arena / 1024, mi.uordblks / 1024, mi.fordblks / 1024, mi.ordblks,
            (unsigned)(((u32)SYS_GetArena2Hi() - (u32)SYS_GetArena2Lo()) / 1024));
    for (int k = 0; k < ns; k++)
        fprintf(f, "  site %4u bytes=%7u cnt=%5u d=%+7d allocs=%6u lr=%08x %08x %08x %08x %08x %08x %08x %08x\n", ids[k], snap[k].bytes, snap[k].cnt,
                (int)snap[k].bytes - (int)snap[k].prev_bytes, snap[k].allocs, snap[k].lr[0], snap[k].lr[1], snap[k].lr[2], snap[k].lr[3], snap[k].lr[4], snap[k].lr[5], snap[k].lr[6], snap[k].lr[7]);
    fflush(f); fsync(fileno(f));   /* que el log sobreviva si Dolphin se cierra a la fuerza */
}
#endif
