/*
 * fsguard.c - protege mkdir()/rename() de libsysbase contra dispositivos no montados.
 *
 * Bug (crash DSI en Wii real, KickWii, PC 802b1a3c en mkdir):
 * mkdir() de libsysbase hace devoptab_list[FindDevice(path)]->mkdir_r sin
 * comprobar que FindDevice devolvio -1. Si "sd:" no esta montado (sin SD, SD
 * que no inicia, o solo hay un disco USB), lee devoptab_list[-1] == NULL y
 * accede a NULL+0x34. rename() tiene el mismo problema (NULL+0x30).
 * Se enlaza con -Wl,--wrap=mkdir,--wrap=_rename_r: toda llamada (nuestra o de
 * librerias) pasa por aqui y devuelve -1/ENODEV en vez de colgar la Wii.
 */
#include <errno.h>
#include <reent.h>
#include <sys/stat.h>
#include <sys/iosupport.h>

int __real_mkdir(const char *path, mode_t mode);
int __real__rename_r(struct _reent *r, const char *a, const char *b);

static int dev_ok(const char *p) {
    if (!p || !*p) return 0;
    int d = FindDevice(p);
    if (d < 0 || d >= STD_MAX) return 0;
    const devoptab_t *t = devoptab_list[d];
    return t != 0;
}

int __wrap_mkdir(const char *path, mode_t mode) {
    if (!dev_ok(path)) { errno = ENODEV; return -1; }
    if (!devoptab_list[FindDevice(path)]->mkdir_r) { errno = ENOSYS; return -1; }
    return __real_mkdir(path, mode);
}

int __wrap__rename_r(struct _reent *r, const char *a, const char *b) {
    if (!dev_ok(a) || !dev_ok(b)) { if (r) r->_errno = ENODEV; return -1; }
    return __real__rename_r(r, a, b);
}

/* 1 si el dispositivo de la ruta ("sd:/..." o "usb:/...") esta montado. */
int fs_dev_mounted(const char *path) { return dev_ok(path); }

/* carpetas de datos de la app en los dispositivos montados, SD primero
 * y despues USB. Con create!=0 crea <dev>:/apps y <dev>:/apps/<app> (mkdir
 * ya pasa por el wrapper, asi que un dispositivo ausente nunca cuelga). */
#include <stdio.h>
#include <string.h>
int fs_data_dirs(const char *app, char dirs[2][96], int create) {
    static const char *devs[] = { "sd:/", "usb:/" };
    int k = 0;
    for (int i = 0; i < 2; i++) {
        if (!dev_ok(devs[i])) continue;
        snprintf(dirs[k], 96, "%sapps/%s/", devs[i], app);
        if (create) {
            char p[96];
            snprintf(p, sizeof(p), "%sapps", devs[i]); mkdir(p, 0777);
            snprintf(p, sizeof(p), "%.*s", (int)strlen(dirs[k]) - 1, dirs[k]); mkdir(p, 0777);
        }
        k++;
    }
    return k;
}
