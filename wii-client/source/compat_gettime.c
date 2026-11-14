/*
 * Compat: la libmbedcrypto precompilada referencia el simbolo externo
 * `gettime`, que en esta libogc es solo una funcion inline (MK_INLINE).
 * Aqui se exporta una version real con el mismo comportamiento.
 */
#include <gctypes.h>
#include <ogc/machine/processor.h>

u64 gettime(void);
u64 gettime(void) {
    u32 hi, lo, hi2;
    do {
        asm volatile("mftbu %0" : "=r"(hi));
        asm volatile("mftb %0"  : "=r"(lo));
        asm volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((u64)hi << 32) | lo;
}
