#ifndef _ASMARM_PGTABLE_H
#define _ASMARM_PGTABLE_H

#include <asm/proc-fns.h>
#include <asm/proc/pgtable.h>

#define module_map	vmalloc
#define module_unmap	vfree

extern int do_check_pgt_cache(int, int);

#define flush_dcache_page(page)			do { } while (0)

/* Needs to be defined here and not in linux/mm.h, as it is arch dependent */
#define PageSkip(page)			(0)
#define kern_addr_valid(addr)		(1)

#endif /* _ASMARM_PGTABLE_H */
