/*
 * MEM2 helpers for TwitchWii 1.2-rc1.
 * Huge FFmpeg TX tables are linker-placed in a MEM2 NOLOAD section
 * (0x92000000). The System Menu / HBC DOL loader is not relied on to
 * load or zero that section. twitchwii_mem2_init() zeros it and shrinks
 * Arena2 so the MEM2 heap cannot overlap the tables. The software
 * canvas is allocated from the remaining MEM2 arena at gfx_init.
 */
#ifndef TWITCHWII_MEM2_H
#define TWITCHWII_MEM2_H

#include <gctypes.h>

void  twitchwii_mem2_init(void);
void *twitchwii_mem2_alloc(u32 size, u32 align);
u32   twitchwii_mem2_bss_start(void);
u32   twitchwii_mem2_bss_end(void);
u32   twitchwii_mem2_bss_size(void);

#endif
