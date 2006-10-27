/*
 * QEMU 8259 interrupt controller emulation
 * 
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2005 Intel Corperation
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <xen/config.h>
#include <xen/types.h>
#include <xen/mm.h>
#include <xen/xmalloc.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/current.h>

/* set irq level. If an edge is detected, then the IRR is set to 1 */
static inline void pic_set_irq1(PicState *s, int irq, int level)
{
    int mask;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    mask = 1 << irq;
    if (s->elcr & mask) {
        /* level triggered */
        if (level) {
            s->irr |= mask;
            s->last_irr |= mask;
        } else {
            s->irr &= ~mask;
            s->last_irr &= ~mask;
        }
    } else {
        /* edge triggered */
        if (level) {
            if ((s->last_irr & mask) == 0)
                s->irr |= mask;
            s->last_irr |= mask;
        } else {
            s->last_irr &= ~mask;
        }
    }
}

/* return the highest priority found in mask (highest = smallest
   number). Return 8 if no irq */
static inline int get_priority(PicState *s, int mask)
{
    int priority;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    if (mask == 0)
        return 8;
    priority = 0;
    while ((mask & (1 << ((priority + s->priority_add) & 7))) == 0)
        priority++;
    return priority;
}

/* return the pic wanted interrupt. return -1 if none */
static int pic_get_irq(PicState *s)
{
    int mask, cur_priority, priority;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    mask = (s->irr|s->irr_xen) & ~s->imr;
    priority = get_priority(s, mask);
    if (priority == 8)
        return -1;
    /* compute current priority. If special fully nested mode on the
       master, the IRQ coming from the slave is not taken into account
       for the priority computation. */
    mask = s->isr;
    if (s->special_fully_nested_mode && s == &s->pics_state->pics[0])
        mask &= ~(1 << 2);
    cur_priority = get_priority(s, mask);
    if (priority < cur_priority) {
        /* higher priority found: an irq should be generated */
        return (priority + s->priority_add) & 7;
    } else {
        return -1;
    }
}

/* raise irq to CPU if necessary. must be called every time the active
   irq may change */
/* XXX: should not export it, but it is needed for an APIC kludge */
void pic_update_irq(struct hvm_virpic *s)
{
    int irq2, irq;

    ASSERT(spin_is_locked(&s->lock));

    /* first look at slave pic */
    irq2 = pic_get_irq(&s->pics[1]);
    if (irq2 >= 0) {
        /* if irq request by slave pic, signal master PIC */
        pic_set_irq1(&s->pics[0], 2, 1);
        pic_set_irq1(&s->pics[0], 2, 0);
    }
    /* look at requested irq */
    irq = pic_get_irq(&s->pics[0]);
    if (irq >= 0) {
        s->irq_request(s->irq_request_opaque, 1);
    }
}

void pic_set_xen_irq(void *opaque, int irq, int level)
{
    struct hvm_virpic *s = opaque;
    unsigned long flags;
    PicState *ps;

    spin_lock_irqsave(&s->lock, flags);

    hvm_vioapic_set_xen_irq(current->domain, irq, level);

    /* Set it on the 8259s */
    ps = &s->pics[irq >> 3];
    if (!(ps->elcr & (1 << (irq & 7))))
	gdprintk(XENLOG_WARNING, "edge-triggered override IRQ?\n");
    if (level) {
	ps->irr_xen |= 1 << (irq & 7);
    } else {
	ps->irr_xen &= ~(1 << (irq & 7));
    }

    pic_update_irq(s);
    spin_unlock_irqrestore(&s->lock, flags);
}

void pic_set_irq_new(void *opaque, int irq, int level)
{
    struct hvm_virpic *s = opaque;
    unsigned long flags;

    spin_lock_irqsave(&s->lock, flags);
    hvm_vioapic_set_irq(current->domain, irq, level);
    pic_set_irq1(&s->pics[irq >> 3], irq & 7, level);
    pic_update_irq(s);
    spin_unlock_irqrestore(&s->lock, flags);
}

void do_pic_irqs (struct hvm_virpic *s, uint16_t irqs)
{
    unsigned long flags;

    spin_lock_irqsave(&s->lock, flags);
    s->pics[1].irr |= (uint8_t)(irqs >> 8);
    s->pics[0].irr |= (uint8_t) irqs;
    hvm_vioapic_do_irqs(current->domain, irqs);
    pic_update_irq(s);
    spin_unlock_irqrestore(&s->lock, flags);
}

void do_pic_irqs_clear (struct hvm_virpic *s, uint16_t irqs)
{
    unsigned long flags;

    spin_lock_irqsave(&s->lock, flags);
    s->pics[1].irr &= ~(uint8_t)(irqs >> 8);
    s->pics[0].irr &= ~(uint8_t) irqs;
    hvm_vioapic_do_irqs_clear(current->domain, irqs);
    pic_update_irq(s);
    spin_unlock_irqrestore(&s->lock, flags);
}

/* obsolete function */
void pic_set_irq(struct hvm_virpic *isa_pic, int irq, int level)
{
    pic_set_irq_new(isa_pic, irq, level);
}

/* acknowledge interrupt 'irq' */
static inline void pic_intack(PicState *s, int irq)
{
    ASSERT(spin_is_locked(&s->pics_state->lock));

    if (s->auto_eoi) {
        if (s->rotate_on_auto_eoi)
            s->priority_add = (irq + 1) & 7;
    } else {
        s->isr |= (1 << irq);
    }
    /* We don't clear a level sensitive interrupt here */
    if (!(s->elcr & (1 << irq)))
        s->irr &= ~(1 << irq);
}

int pic_read_irq(struct hvm_virpic *s)
{
    int irq, irq2, intno;
    unsigned long flags;

    spin_lock_irqsave(&s->lock, flags);
    irq = pic_get_irq(&s->pics[0]);
    if (irq >= 0) {
        pic_intack(&s->pics[0], irq);
        if (irq == 2) {
            irq2 = pic_get_irq(&s->pics[1]);
            if (irq2 >= 0) {
                pic_intack(&s->pics[1], irq2);
            } else {
                /* spurious IRQ on slave controller */
                irq2 = 7;
            }
            intno = s->pics[1].irq_base + irq2;
            irq = irq2 + 8;
        } else {
            intno = s->pics[0].irq_base + irq;
        }
    } else {
        /* spurious IRQ on host controller */
        irq = 7;
        intno = s->pics[0].irq_base + irq;
    }
    pic_update_irq(s);
    spin_unlock_irqrestore(&s->lock, flags);
        
    return intno;
}

static void update_shared_irr(struct hvm_virpic *s, PicState *c)
{
    uint8_t *pl, *pe;

    ASSERT(spin_is_locked(&s->lock));

    get_sp(current->domain)->sp_global.pic_elcr = 
        s->pics[0].elcr | ((u16)s->pics[1].elcr << 8);
    pl =(uint8_t*)&get_sp(current->domain)->sp_global.pic_last_irr;
    pe =(uint8_t*)&get_sp(current->domain)->sp_global.pic_elcr;
    if ( c == &s->pics[0] ) {
         *pl = c->last_irr;
         *pe = c->elcr;
    }
    else {
         *(pl+1) = c->last_irr;
         *(pe+1) = c->elcr;
    }
}

static void pic_reset(void *opaque)
{
    PicState *s = opaque;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    s->last_irr = 0;
    s->irr = 0;
    s->imr = 0;
    s->isr = 0;
    s->priority_add = 0;
    s->irq_base = 0;
    s->read_reg_select = 0;
    s->poll = 0;
    s->special_mask = 0;
    s->init_state = 0;
    s->auto_eoi = 0;
    s->rotate_on_auto_eoi = 0;
    s->special_fully_nested_mode = 0;
    s->init4 = 0;
    /* Note: ELCR is not reset */
}

static void pic_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    PicState *s = opaque;
    int priority, cmd, irq;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    addr &= 1;
    if (addr == 0) {
        if (val & 0x10) {
            /* init */
            pic_reset(s);
            update_shared_irr(s->pics_state, s);
            /* deassert a pending interrupt */
            s->pics_state->irq_request(s->pics_state->irq_request_opaque, 0);
            s->init_state = 1;
            s->init4 = val & 1;
            if (val & 0x02)
                hw_error("single mode not supported");
            if (val & 0x08)
                hw_error("level sensitive irq not supported");
        } else if (val & 0x08) {
            if (val & 0x04)
                s->poll = 1;
            if (val & 0x02)
                s->read_reg_select = val & 1;
            if (val & 0x40)
                s->special_mask = (val >> 5) & 1;
        } else {
            cmd = val >> 5;
            switch(cmd) {
            case 0:
            case 4:
                s->rotate_on_auto_eoi = cmd >> 2;
                break;
            case 1: /* end of interrupt */
            case 5:
                priority = get_priority(s, s->isr);
                if (priority != 8) {
                    irq = (priority + s->priority_add) & 7;
                    s->isr &= ~(1 << irq);
                    if (cmd == 5)
                        s->priority_add = (irq + 1) & 7;
                    pic_update_irq(s->pics_state);
                }
                break;
            case 3:
                irq = val & 7;
                s->isr &= ~(1 << irq);
                pic_update_irq(s->pics_state);
                break;
            case 6:
                s->priority_add = (val + 1) & 7;
                pic_update_irq(s->pics_state);
                break;
            case 7:
                irq = val & 7;
                s->isr &= ~(1 << irq);
                s->priority_add = (irq + 1) & 7;
                pic_update_irq(s->pics_state);
                break;
            default:
                /* no operation */
                break;
            }
        }
    } else {
        switch(s->init_state) {
        case 0:
            /* normal mode */
            s->imr = val;
            pic_update_irq(s->pics_state);
            break;
        case 1:
            s->irq_base = val & 0xf8;
            s->init_state = 2;
            break;
        case 2:
            if (s->init4) {
                s->init_state = 3;
            } else {
                s->init_state = 0;
            }
            break;
        case 3:
            s->special_fully_nested_mode = (val >> 4) & 1;
            s->auto_eoi = (val >> 1) & 1;
            s->init_state = 0;
            break;
        }
    }
}

static uint32_t pic_poll_read (PicState *s, uint32_t addr1)
{
    int ret;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    ret = pic_get_irq(s);
    if (ret >= 0) {
        if (addr1 >> 7) {
            s->pics_state->pics[0].isr &= ~(1 << 2);
            s->pics_state->pics[0].irr &= ~(1 << 2);
        }
        s->irr &= ~(1 << ret);
        s->irr_xen &= ~(1 << ret);
        s->isr &= ~(1 << ret);
        if (addr1 >> 7 || ret != 2)
            pic_update_irq(s->pics_state);
    } else {
        ret = 0x07;
        pic_update_irq(s->pics_state);
    }

    return ret;
}

static uint32_t pic_ioport_read(void *opaque, uint32_t addr1)
{
    PicState *s = opaque;
    unsigned int addr;
    int ret;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    addr = addr1;
    addr &= 1;
    if (s->poll) {
        ret = pic_poll_read(s, addr1);
        s->poll = 0;
    } else {
        if (addr == 0) {
            if (s->read_reg_select)
                ret = s->isr;
            else
                ret = s->irr | s->irr_xen;
        } else {
            ret = s->imr;
        }
    }
    return ret;
}

/* memory mapped interrupt status */
/* XXX: may be the same than pic_read_rq() */
uint32_t pic_intack_read(struct hvm_virpic *s)
{
    int ret;
    unsigned long flags;

    spin_lock_irqsave(&s->lock, flags);
    ret = pic_poll_read(&s->pics[0], 0x00);
    if (ret == 2)
        ret = pic_poll_read(&s->pics[1], 0x80) + 8;
    /* Prepare for ISR read */
    s->pics[0].read_reg_select = 1;
    spin_unlock_irqrestore(&s->lock, flags);
    
    return ret;
}

static void elcr_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    PicState *s = opaque;

    ASSERT(spin_is_locked(&s->pics_state->lock));

    s->elcr = val & s->elcr_mask;
}

static uint32_t elcr_ioport_read(void *opaque, uint32_t addr1)
{
    PicState *s = opaque;
    return s->elcr;
}

/* XXX: add generic master/slave system */
static void pic_init1(int io_addr, int elcr_addr, PicState *s)
{
    ASSERT(spin_is_locked(&s->pics_state->lock));

    pic_reset(s);

    /* XXX We set the ELCR to level triggered here, but that should
       really be done by the BIOS, and only for PCI IRQs. */
    s->elcr = 0xff & s->elcr_mask;
}

void pic_init(struct hvm_virpic *s, void (*irq_request)(void *, int),
              void *irq_request_opaque)
{
    unsigned long flags;

    memset(s, 0, sizeof(*s));
    spin_lock_init(&s->lock);
    s->pics[0].pics_state = s;
    s->pics[1].pics_state = s;
    s->pics[0].elcr_mask = 0xf8;
    s->pics[1].elcr_mask = 0xde;
    spin_lock_irqsave(&s->lock, flags);
    pic_init1(0x20, 0x4d0, &s->pics[0]);
    pic_init1(0xa0, 0x4d1, &s->pics[1]);
    spin_unlock_irqrestore(&s->lock, flags);
    s->irq_request = irq_request;
    s->irq_request_opaque = irq_request_opaque;
}

static int intercept_pic_io(ioreq_t *p)
{
    struct hvm_virpic *pic;
    uint32_t data;
    unsigned long flags;

    if ( p->size != 1 || p->count != 1 ) {
        printk("PIC_IO wrong access size %d!\n", (int)p->size);
        return 1;
    }

    pic = &current->domain->arch.hvm_domain.vpic;
    if ( p->dir == IOREQ_WRITE ) {
        if ( p->pdata_valid )
            (void)hvm_copy_from_guest_phys(
                &data, (unsigned long)p->u.pdata, p->size);
        else
            data = p->u.data;
        spin_lock_irqsave(&pic->lock, flags);
        pic_ioport_write((void*)&pic->pics[p->addr>>7],
                (uint32_t) p->addr, (uint32_t) (data & 0xff));
        spin_unlock_irqrestore(&pic->lock, flags);
    }
    else {
        spin_lock_irqsave(&pic->lock, flags);
        data = pic_ioport_read(
            (void*)&pic->pics[p->addr>>7], (uint32_t) p->addr);
        spin_unlock_irqrestore(&pic->lock, flags);
        if ( p->pdata_valid )
            (void)hvm_copy_to_guest_phys(
                (unsigned long)p->u.pdata, &data, p->size);
        else
            p->u.data = (u64)data;
    }
    return 1;
}

static int intercept_elcr_io(ioreq_t *p)
{
    struct hvm_virpic *s;
    uint32_t data;
    unsigned long flags;

    if ( p->size != 1 || p->count != 1 ) {
        printk("PIC_IO wrong access size %d!\n", (int)p->size);
        return 1;
    }

    s = &current->domain->arch.hvm_domain.vpic;
    if ( p->dir == IOREQ_WRITE ) {
        if ( p->pdata_valid )
            (void)hvm_copy_from_guest_phys(
                &data, (unsigned long)p->u.pdata, p->size);
        else
            data = p->u.data;
        spin_lock_irqsave(&s->lock, flags);
        elcr_ioport_write((void*)&s->pics[p->addr&1],
                (uint32_t) p->addr, (uint32_t)( data & 0xff));
        get_sp(current->domain)->sp_global.pic_elcr =
            s->pics[0].elcr | ((u16)s->pics[1].elcr << 8);
        spin_unlock_irqrestore(&s->lock, flags);
    }
    else {
        data = (u64) elcr_ioport_read(
                (void*)&s->pics[p->addr&1], (uint32_t) p->addr);
        if ( p->pdata_valid )
            (void)hvm_copy_to_guest_phys(
                (unsigned long)p->u.pdata, &data, p->size);
        else
            p->u.data = (u64)data;
    }
    return 1;
}

void register_pic_io_hook (void)
{
    register_portio_handler(0x20, 2, intercept_pic_io);
    register_portio_handler(0x4d0, 1, intercept_elcr_io);
    register_portio_handler(0xa0, 2, intercept_pic_io);
    register_portio_handler(0x4d1, 1, intercept_elcr_io);
}


/* IRQ handling */
int cpu_get_pic_interrupt(struct vcpu *v, int *type)
{
    int intno;
    struct hvm_virpic *s = &v->domain->arch.hvm_domain.vpic;
    struct hvm_domain *plat = &v->domain->arch.hvm_domain;

    if ( !vlapic_accept_pic_intr(v) )
        return -1;

    if (cmpxchg(&plat->interrupt_request, 1, 0) != 1)
        return -1;

    /* read the irq from the PIC */
    intno = pic_read_irq(s);
    *type = APIC_DM_EXTINT;
    return intno;
}

int is_periodic_irq(struct vcpu *v, int irq, int type)
{
    int vec;
    struct periodic_time *pt =
        &(v->domain->arch.hvm_domain.pl_time.periodic_tm);
    struct RTCState *vrtc =
        &(v->domain->arch.hvm_domain.pl_time.vrtc);

    if (pt->irq == 0) { /* Is it pit irq? */
        if (type == APIC_DM_EXTINT)
            vec = v->domain->arch.hvm_domain.vpic.pics[0].irq_base;
        else
            vec =
              v->domain->arch.hvm_domain.vioapic.redirtbl[0].RedirForm.vector;

        if (irq == vec)
            return 1;
    }

    if (pt->irq == 8) { /* Or rtc irq? */
        if (type == APIC_DM_EXTINT)
            vec = v->domain->arch.hvm_domain.vpic.pics[1].irq_base;
        else
            vec =
              v->domain->arch.hvm_domain.vioapic.redirtbl[8].RedirForm.vector;

        if (irq == vec)
            return is_rtc_periodic_irq(vrtc);
    }

    return 0;
}

int is_irq_enabled(struct vcpu *v, int irq)
{
    struct hvm_vioapic *vioapic = &v->domain->arch.hvm_domain.vioapic;
    struct hvm_virpic *vpic=&v->domain->arch.hvm_domain.vpic;

    if (vioapic->redirtbl[irq].RedirForm.mask == 0)
       return 1;

    if ( irq & 8 ) {
        return !( (1 << (irq&7)) & vpic->pics[1].imr);
    }
    else {
        return !( (1 << irq) & vpic->pics[0].imr);
    }
}

