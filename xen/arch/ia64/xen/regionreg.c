/*
 * Region register and region id management
 *
 * Copyright (C) 2001-2004 Hewlett-Packard Co.
 *	Dan Magenheimer (dan.magenheimer@hp.com
 *	Bret Mckee (bret.mckee@hp.com)
 *
 */


#include <linux/config.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <asm/page.h>
#include <asm/regionreg.h>
#include <asm/vhpt.h>
#include <asm/vcpu.h>
extern void ia64_new_rr7(unsigned long rid,void *shared_info, void *shared_arch_info, unsigned long p_vhpt, unsigned long v_pal);
extern void *pal_vaddr;

/* FIXME: where these declarations should be there ? */
extern void panic_domain(struct pt_regs *, const char *, ...);

#define DOMAIN_RID_BITS_DEFAULT 18

#define	IA64_MIN_IMPL_RID_BITS	(IA64_MIN_IMPL_RID_MSB+1)
#define	IA64_MAX_IMPL_RID_BITS	24

#define MIN_RIDS	(1 << IA64_MIN_IMPL_RID_BITS)
#define	MIN_RID_MAX	(MIN_RIDS - 1)
#define	MIN_RID_MASK	(MIN_RIDS - 1)
#define	MAX_RIDS	(1 << (IA64_MAX_IMPL_RID_BITS))
#define	MAX_RID		(MAX_RIDS - 1)
#define	MAX_RID_BLOCKS	(1 << (IA64_MAX_IMPL_RID_BITS-IA64_MIN_IMPL_RID_BITS))
#define RIDS_PER_RIDBLOCK MIN_RIDS

#if 0
// following already defined in include/asm-ia64/gcc_intrin.h
// it should probably be ifdef'd out from there to ensure all region
// register usage is encapsulated in this file
static inline unsigned long
ia64_get_rr (unsigned long rr)
{
	    unsigned long r;
	    __asm__ __volatile__ (";;mov %0=rr[%1];;":"=r"(r):"r"(rr):"memory");
	    return r;
}

static inline void
ia64_set_rr (unsigned long rr, unsigned long rrv)
{
	    __asm__ __volatile__ (";;mov rr[%0]=%1;;"::"r"(rr),"r"(rrv):"memory");
}
#endif

static unsigned long allocate_metaphysical_rr(struct domain *d, int n)
{
	ia64_rr rrv;

	rrv.rrval = 0;	// Or else may see reserved bit fault
	rrv.rid = d->arch.starting_mp_rid + n;
	rrv.ps = PAGE_SHIFT;
	rrv.ve = 0;
	/* Mangle metaphysical rid */
	rrv.rrval = vmMangleRID(rrv.rrval);
	return rrv.rrval;
}

/*************************************
  Region Block setup/management
*************************************/

static int implemented_rid_bits = 0;
static int mp_rid_shift;
static struct domain *ridblock_owner[MAX_RID_BLOCKS] = { 0 };

void init_rid_allocator (void)
{
	int log_blocks;
	pal_vm_info_2_u_t vm_info_2;

	/* Get machine rid_size.  */
	BUG_ON (ia64_pal_vm_summary (NULL, &vm_info_2) != 0);
	implemented_rid_bits = vm_info_2.pal_vm_info_2_s.rid_size;

	/* We need at least a few space...  */
	BUG_ON (implemented_rid_bits <= IA64_MIN_IMPL_RID_BITS);

	/* And we can accept too much space.  */
	if (implemented_rid_bits > IA64_MAX_IMPL_RID_BITS)
		implemented_rid_bits = IA64_MAX_IMPL_RID_BITS;

	log_blocks = (implemented_rid_bits - IA64_MIN_IMPL_RID_BITS);

	printf ("Maximum of simultaneous domains: %d\n",
		(1 << log_blocks) - 1);

	mp_rid_shift = IA64_MIN_IMPL_RID_BITS - log_blocks;
	BUG_ON (mp_rid_shift < 3);
}


/*
 * Allocate a power-of-two-sized chunk of region id space -- one or more
 *  "rid blocks"
 */
int allocate_rid_range(struct domain *d, unsigned long ridbits)
{
	int i, j, n_rid_blocks;

	if (ridbits == 0)
		ridbits = DOMAIN_RID_BITS_DEFAULT;

	if (ridbits >= IA64_MAX_IMPL_RID_BITS)
		ridbits = IA64_MAX_IMPL_RID_BITS - 1;
	
	if (ridbits < IA64_MIN_IMPL_RID_BITS)
		ridbits = IA64_MIN_IMPL_RID_BITS;

	// convert to rid_blocks and find one
	n_rid_blocks = 1UL << (ridbits - IA64_MIN_IMPL_RID_BITS);
	
	// skip over block 0, reserved for "meta-physical mappings (and Xen)"
	for (i = n_rid_blocks; i < MAX_RID_BLOCKS; i += n_rid_blocks) {
		if (ridblock_owner[i] == NULL) {
			for (j = i; j < i + n_rid_blocks; ++j) {
				if (ridblock_owner[j])
					break;
			}
			if (ridblock_owner[j] == NULL)
				break;
		}
	}
	
	if (i >= MAX_RID_BLOCKS)
		return 0;
	
	// found an unused block:
	//   (i << min_rid_bits) <= rid < ((i + n) << min_rid_bits)
	// mark this block as owned
	for (j = i; j < i + n_rid_blocks; ++j)
		ridblock_owner[j] = d;
	
	// setup domain struct
	d->arch.rid_bits = ridbits;
	d->arch.starting_rid = i << IA64_MIN_IMPL_RID_BITS;
	d->arch.ending_rid = (i+n_rid_blocks) << IA64_MIN_IMPL_RID_BITS;
	
	d->arch.starting_mp_rid = i << mp_rid_shift;
	d->arch.ending_mp_rid = (i + 1) << mp_rid_shift;

	d->arch.metaphysical_rr0 = allocate_metaphysical_rr(d, 0);
	d->arch.metaphysical_rr4 = allocate_metaphysical_rr(d, 1);

	printf("###allocating rid_range, domain %p: rid=%x-%x mp_rid=%x\n",
	       d, d->arch.starting_rid, d->arch.ending_rid,
	       d->arch.starting_mp_rid);
	
	return 1;
}


int deallocate_rid_range(struct domain *d)
{
	int i;
	int rid_block_end = d->arch.ending_rid >> IA64_MIN_IMPL_RID_BITS;
	int rid_block_start = d->arch.starting_rid >> IA64_MIN_IMPL_RID_BITS;

	//
	// not all domains will have allocated RIDs (physical mode loaders for instance)
	//
	if (d->arch.rid_bits == 0) return 1;

#ifdef DEBUG
	for (i = rid_block_start; i < rid_block_end; ++i) {
	        ASSERT(ridblock_owner[i] == d);
	    }
#endif
	
	for (i = rid_block_start; i < rid_block_end; ++i)
		ridblock_owner[i] = NULL;
	
	d->arch.rid_bits = 0;
	d->arch.starting_rid = 0;
	d->arch.ending_rid = 0;
	d->arch.starting_mp_rid = 0;
	d->arch.ending_mp_rid = 0;
	return 1;
}

static void
set_rr(unsigned long rr, unsigned long rrval)
{
	ia64_set_rr(rr, vmMangleRID(rrval));
	ia64_srlz_d();
}

// validates and changes a single region register
// in the currently executing domain
// Passing a value of -1 is a (successful) no-op
// NOTE: DOES NOT SET VCPU's rrs[x] value!!
int set_one_rr(unsigned long rr, unsigned long val)
{
	struct vcpu *v = current;
	unsigned long rreg = REGION_NUMBER(rr);
	ia64_rr rrv, newrrv, memrrv;
	unsigned long newrid;

	if (val == -1) return 1;

	rrv.rrval = val;
	newrrv.rrval = 0;
	newrid = v->arch.starting_rid + rrv.rid;

	if (newrid > v->arch.ending_rid) {
		printk("can't set rr%d to %lx, starting_rid=%x,"
			"ending_rid=%x, val=%lx\n", (int) rreg, newrid,
			v->arch.starting_rid,v->arch.ending_rid,val);
		return 0;
	}

#if 0
	memrrv.rrval = rrv.rrval;
	if (rreg == 7) {
		newrrv.rid = newrid;
		newrrv.ve = VHPT_ENABLED_REGION_7;
		newrrv.ps = IA64_GRANULE_SHIFT;
		ia64_new_rr7(vmMangleRID(newrrv.rrval),v->vcpu_info,
				v->arch.privregs);
	}
	else {
		newrrv.rid = newrid;
		// FIXME? region 6 needs to be uncached for EFI to work
		if (rreg == 6) newrrv.ve = VHPT_ENABLED_REGION_7;
		else newrrv.ve = VHPT_ENABLED_REGION_0_TO_6;
		newrrv.ps = PAGE_SHIFT;
		if (rreg == 0) v->arch.metaphysical_saved_rr0 = newrrv.rrval;
		set_rr(rr,newrrv.rrval);
	}
#else
	memrrv.rrval = rrv.rrval;
	newrrv.rid = newrid;
	newrrv.ve = 1;  // VHPT now enabled for region 7!!
	newrrv.ps = PAGE_SHIFT;

	if (rreg == 0) {
		v->arch.metaphysical_saved_rr0 = vmMangleRID(newrrv.rrval);
		if (!PSCB(v,metaphysical_mode))
			set_rr(rr,newrrv.rrval);
	} else if (rreg == 7) {
		ia64_new_rr7(vmMangleRID(newrrv.rrval),v->vcpu_info,
			     v->arch.privregs, __get_cpu_var(vhpt_paddr),
			     (unsigned long) pal_vaddr);
	} else {
		set_rr(rr,newrrv.rrval);
	}
#endif
	return 1;
}

// set rr0 to the passed rid (for metaphysical mode so don't use domain offset
int set_metaphysical_rr0(void)
{
	struct vcpu *v = current;
//	ia64_rr rrv;
	
//	rrv.ve = 1; 	FIXME: TURN ME BACK ON WHEN VHPT IS WORKING
	ia64_set_rr(0,v->arch.metaphysical_rr0);
	ia64_srlz_d();
	return 1;
}

void init_all_rr(struct vcpu *v)
{
	ia64_rr rrv;

	rrv.rrval = 0;
	//rrv.rrval = v->domain->arch.metaphysical_rr0;
	rrv.ps = PAGE_SHIFT;
	rrv.ve = 1;
if (!v->vcpu_info) { printf("Stopping in init_all_rr\n"); dummy(); }
	VCPU(v,rrs[0]) = -1;
	VCPU(v,rrs[1]) = rrv.rrval;
	VCPU(v,rrs[2]) = rrv.rrval;
	VCPU(v,rrs[3]) = rrv.rrval;
	VCPU(v,rrs[4]) = rrv.rrval;
	VCPU(v,rrs[5]) = rrv.rrval;
	rrv.ve = 0; 
	VCPU(v,rrs[6]) = rrv.rrval;
//	v->shared_info->arch.rrs[7] = rrv.rrval;
}


/* XEN/ia64 INTERNAL ROUTINES */

// loads a thread's region register (0-6) state into
// the real physical region registers.  Returns the
// (possibly mangled) bits to store into rr7
// iff it is different than what is currently in physical
// rr7 (because we have to to assembly and physical mode
// to change rr7).  If no change to rr7 is required, returns 0.
//
void load_region_regs(struct vcpu *v)
{
	unsigned long rr0, rr1,rr2, rr3, rr4, rr5, rr6, rr7;
	// TODO: These probably should be validated
	unsigned long bad = 0;

	if (VCPU(v,metaphysical_mode)) {
		rr0 = v->domain->arch.metaphysical_rr0;
		ia64_set_rr(0x0000000000000000L, rr0);
		ia64_srlz_d();
	}
	else {
		rr0 =  VCPU(v,rrs[0]);
		if (!set_one_rr(0x0000000000000000L, rr0)) bad |= 1;
	}
	rr1 =  VCPU(v,rrs[1]);
	rr2 =  VCPU(v,rrs[2]);
	rr3 =  VCPU(v,rrs[3]);
	rr4 =  VCPU(v,rrs[4]);
	rr5 =  VCPU(v,rrs[5]);
	rr6 =  VCPU(v,rrs[6]);
	rr7 =  VCPU(v,rrs[7]);
	if (!set_one_rr(0x2000000000000000L, rr1)) bad |= 2;
	if (!set_one_rr(0x4000000000000000L, rr2)) bad |= 4;
	if (!set_one_rr(0x6000000000000000L, rr3)) bad |= 8;
	if (!set_one_rr(0x8000000000000000L, rr4)) bad |= 0x10;
	if (!set_one_rr(0xa000000000000000L, rr5)) bad |= 0x20;
	if (!set_one_rr(0xc000000000000000L, rr6)) bad |= 0x40;
	if (!set_one_rr(0xe000000000000000L, rr7)) bad |= 0x80;
	if (bad) {
		panic_domain(0,"load_region_regs: can't set! bad=%lx\n",bad);
	}
}
