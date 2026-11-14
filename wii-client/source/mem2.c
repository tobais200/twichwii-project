#include <string.h>
#include <gccore.h>
#include <ogc/cache.h>
#include "mem2.h"

extern unsigned char __bss_mem2_start[];
extern unsigned char __bss_mem2_end[];

/* Present in libogc, not declared in system.h of this toolchain. */
void *SYS_AllocArena2MemLo(u32 size, u32 align);

static int mem2_ready;

void twitchwii_mem2_init(void)
{
    u32 start, end, size;
    void *hi;

    if (mem2_ready)
        return;

    start = (u32)__bss_mem2_start;
    end   = (u32)__bss_mem2_end;
    if (end < start)
        end = start;
    size = end - start;

    /* SYS_Init has already loaded Arena2 from __Arena2Lo/__Arena2Hi
       (0x90002000 .. 0x933e0000). Pull Hi down to the table block so
       SYS_AllocArena2MemLo / a MEM2 malloc cannot overlap it. */
    hi = SYS_GetArena2Hi();
    if (size && hi && (u32)hi > start)
        SYS_SetArena2Hi((void *)start);

    if (size) {
        memset((void *)start, 0, size);
        DCFlushRange((void *)start, size);
    }
    mem2_ready = 1;
}

void *twitchwii_mem2_alloc(u32 size, u32 align)
{
    void *p, *hi;

    twitchwii_mem2_init();
    if (!size)
        return NULL;
    if (align < 32)
        align = 32;
    p = SYS_AllocArena2MemLo(size, align);
    hi = SYS_GetArena2Hi();
    if (!p || (hi && (u32)p >= (u32)hi))
        return NULL;
    memset(p, 0, size);
    DCFlushRange(p, size);
    return p;
}

u32 twitchwii_mem2_bss_start(void) { return (u32)__bss_mem2_start; }
u32 twitchwii_mem2_bss_end(void)   { return (u32)__bss_mem2_end; }
u32 twitchwii_mem2_bss_size(void)
{
    u32 start = (u32)__bss_mem2_start, end = (u32)__bss_mem2_end;
    return end > start ? end - start : 0;
}
