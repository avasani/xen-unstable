
#ifndef __XEN_SHADOW_H__
#define __XEN_SHADOW_H__

#include <xen/config.h>

#ifdef CONFIG_SHADOW

#include <asm/shadow.h>

#else

#define shadow_drop_references(_d, _p)          ((void)0)
#define shadow_sync_and_drop_references(_d, _p) ((void)0)
#define shadow_tainted_refcnts(_d)              (0)

#endif

#endif /* __XEN_SHADOW_H__ */
