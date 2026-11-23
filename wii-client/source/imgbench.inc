/* IMGBENCH (solo pruebas en Dolphin): si existe <datos>/imgbench.txt, la UI no pide
 * imagenes y este hilo pide la lista (como si estuviera en pantalla), 2 pasadas:
 * 1) en frio, 2) despues de vaciar la RAM (con cache de disco si existe). */
static char bench_urls[48][768]; static int bench_n;
static void *bench_thread(void *a) {
    (void)a;
    sleep(BENCH_WAIT);
    for (int pass = 1; pass <= 2; pass++) {
        u64 s = gettime(); int ready = 0; int done[48] = { 0 };
        TLOG("BENCH pass %d start n=%d", pass, bench_n);
        while (ticks_to_millisecs(gettime() - s) < 120000) {
            ready = 0;
            for (int i = 0; i < bench_n; i++) {
                if (done[i]) { ready++; get_impl(bench_urls[i], 0); continue; }
                if (get_impl(bench_urls[i], 0)) { done[i] = 1; ready++; TLOG("BENCH pass %d ready %d at %ums %s", pass, i, (unsigned)ticks_to_millisecs(gettime() - s), bench_urls[i]); }
            }
            if (ready == bench_n) break;
            usleep(20000);
        }
        TLOG("BENCH pass %d DONE %d/%d in %ums", pass, ready, bench_n, (unsigned)ticks_to_millisecs(gettime() - s));
        thumbs_flush();
        sleep(2);
    }
    TLOG("BENCH END");
    return NULL;
}
static void bench_start(void) {
    char d[2][96], fn[128], line[800]; lwp_t th;
    if (fs_data_dirs(TV_APP_NAME, d, 0) < 1) return;
    snprintf(fn, sizeof(fn), "%simgbench.txt", d[0]);
    FILE *f = fopen(fn, "r"); if (!f) return;
    while (bench_n < 48 && fgets(line, sizeof(line), f)) { line[strcspn(line, "\r\n")] = 0; if (line[0] == 'h') snprintf(bench_urls[bench_n++], 768, "%s", line); }
    fclose(f);
    if (!bench_n) return;
    bench_on = 1;
    LWP_CreateThread(&th, bench_thread, NULL, NULL, 32 * 1024, 40);
}
