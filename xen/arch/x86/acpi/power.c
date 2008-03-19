/* drivers/acpi/sleep/power.c - PM core functionality for Xen
 *
 * Copyrights from Linux side:
 * Copyright (c) 2000-2003 Patrick Mochel
 * Copyright (C) 2001-2003 Pavel Machek <pavel@suse.cz>
 * Copyright (c) 2003 Open Source Development Lab
 * Copyright (c) 2004 David Shaohua Li <shaohua.li@intel.com>
 * Copyright (c) 2005 Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>
 *
 * Slimmed with Xen specific support.
 */

#include <xen/config.h>
#include <asm/io.h>
#include <asm/acpi.h>
#include <xen/acpi.h>
#include <xen/errno.h>
#include <xen/iocap.h>
#include <xen/sched.h>
#include <asm/acpi.h>
#include <asm/irq.h>
#include <asm/init.h>
#include <xen/spinlock.h>
#include <xen/sched.h>
#include <xen/domain.h>
#include <xen/console.h>
#include <public/platform.h>
#include <asm/tboot.h>

#define pmprintk(_l, _f, _a...) printk(_l "<PM> " _f "\n", ## _a )

static char opt_acpi_sleep[20];
string_param("acpi_sleep", opt_acpi_sleep);

static u8 sleep_states[ACPI_S_STATE_COUNT];
static DEFINE_SPINLOCK(pm_lock);

struct acpi_sleep_info acpi_sinfo;

void do_suspend_lowlevel(void);

static int device_power_down(void)
{
    console_suspend();

    time_suspend();

    i8259A_suspend();
    
    ioapic_suspend();
    
    lapic_suspend();

    return 0;
}

static void device_power_up(void)
{
    lapic_resume();
    
    ioapic_resume();

    i8259A_resume();
    
    time_resume();

    console_resume();
}

static void freeze_domains(void)
{
    struct domain *d;

    for_each_domain ( d )
        if ( d->domain_id != 0 )
            domain_pause(d);
}

static void thaw_domains(void)
{
    struct domain *d;

    for_each_domain ( d )
        if ( d->domain_id != 0 )
            domain_unpause(d);
}

static void acpi_sleep_prepare(u32 state)
{
    void *wakeup_vector_va;

    if ( state != ACPI_STATE_S3 )
        return;

    wakeup_vector_va = __acpi_map_table(
        acpi_sinfo.wakeup_vector, sizeof(uint64_t));
    if ( acpi_sinfo.vector_width == 32 )
    {
            *(uint32_t *)wakeup_vector_va =
                tboot_in_measured_env() ?
                (uint32_t)g_tboot_shared->s3_tb_wakeup_entry :
                (uint32_t)bootsym_phys(wakeup_start);
    }
    else
    {
            *(uint64_t *)wakeup_vector_va =
                tboot_in_measured_env() ?
                (uint64_t)g_tboot_shared->s3_tb_wakeup_entry :
                (uint64_t)bootsym_phys(wakeup_start);
    }
}

static void acpi_sleep_post(u32 state) {}

/* Main interface to do xen specific suspend/resume */
static int enter_state(u32 state)
{
    unsigned long flags;
    int error;

    if ( (state <= ACPI_STATE_S0) || (state > ACPI_S_STATES_MAX) )
        return -EINVAL;

    if ( !spin_trylock(&pm_lock) )
        return -EBUSY;

    pmprintk(XENLOG_INFO, "Preparing system for ACPI S%d state.", state);

    freeze_domains();

    disable_nonboot_cpus();
    if ( num_online_cpus() != 1 )
    {
        error = -EBUSY;
        goto enable_cpu;
    }

    hvm_cpu_down();

    acpi_sleep_prepare(state);

    local_irq_save(flags);

    if ( (error = device_power_down()) )
    {
        pmprintk(XENLOG_ERR, "Some devices failed to power down.");
        goto done;
    }

    ACPI_FLUSH_CPU_CACHE();

    switch ( state )
    {
    case ACPI_STATE_S3:
        do_suspend_lowlevel();
        break;
    case ACPI_STATE_S5:
        acpi_enter_sleep_state(ACPI_STATE_S5);
        break;
    default:
        error = -EINVAL;
        break;
    }

    pmprintk(XENLOG_DEBUG, "Back to C.");

    /* Restore CR4 and EFER from cached values. */
    write_cr4(read_cr4());
    if ( cpu_has_efer )
        write_efer(read_efer());

    device_power_up();

    pmprintk(XENLOG_INFO, "Finishing wakeup from ACPI S%d state.", state);

 done:
    local_irq_restore(flags);
    acpi_sleep_post(state);
    if ( !hvm_cpu_up() )
        BUG();

 enable_cpu:
    enable_nonboot_cpus();
    thaw_domains();
    spin_unlock(&pm_lock);
    return error;
}

static long enter_state_helper(void *data)
{
    struct acpi_sleep_info *sinfo = (struct acpi_sleep_info *)data;
    return enter_state(sinfo->sleep_state);
}

/*
 * Dom0 issues this hypercall in place of writing pm1a_cnt. Xen then
 * takes over the control and put the system into sleep state really.
 */
int acpi_enter_sleep(struct xenpf_enter_acpi_sleep *sleep)
{
    if ( !IS_PRIV(current->domain) || !acpi_sinfo.pm1a_cnt_blk.address )
        return -EPERM;

    /* Sanity check */
    if ( acpi_sinfo.pm1b_cnt_val &&
         ((sleep->pm1a_cnt_val ^ sleep->pm1b_cnt_val) &
          ACPI_BITMASK_SLEEP_ENABLE) )
    {
        pmprintk(XENLOG_ERR, "Mismatched pm1a/pm1b setting.");
        return -EINVAL;
    }

    if ( sleep->flags )
        return -EINVAL;

    acpi_sinfo.pm1a_cnt_val = sleep->pm1a_cnt_val;
    acpi_sinfo.pm1b_cnt_val = sleep->pm1b_cnt_val;
    acpi_sinfo.sleep_state = sleep->sleep_state;

    return continue_hypercall_on_cpu(0, enter_state_helper, &acpi_sinfo);
}

static int acpi_get_wake_status(void)
{
    uint32_t val;
    acpi_status status;

    /* Wake status is the 15th bit of PM1 status register. (ACPI spec 3.0) */
    status = acpi_hw_register_read(ACPI_REGISTER_PM1_STATUS, &val);
    if ( ACPI_FAILURE(status) )
        return 0;

    val &= ACPI_BITMASK_WAKE_STATUS;
    val >>= ACPI_BITPOSITION_WAKE_STATUS;
    return val;
}

static void tboot_sleep(u8 sleep_state)
{
   uint32_t shutdown_type;
   
   *((struct acpi_sleep_info *)(unsigned long)g_tboot_shared->acpi_sinfo) =
       acpi_sinfo;

   switch ( sleep_state )
   {
       case ACPI_STATE_S3:
           shutdown_type = TB_SHUTDOWN_S3;
           g_tboot_shared->s3_k_wakeup_entry =
               (uint32_t)bootsym_phys(wakeup_start);
           break;
       case ACPI_STATE_S4:
           shutdown_type = TB_SHUTDOWN_S4;
           break;
       case ACPI_STATE_S5:
           shutdown_type = TB_SHUTDOWN_S5;
           break;
       default:
           return;
   }

   tboot_shutdown(shutdown_type);
}
         
/* System is really put into sleep state by this stub */
acpi_status asmlinkage acpi_enter_sleep_state(u8 sleep_state)
{
    acpi_status status;

    if ( tboot_in_measured_env() )
    {
        tboot_sleep(sleep_state);
        pmprintk(XENLOG_ERR, "TBOOT failed entering s3 state\n");
        return_ACPI_STATUS(AE_ERROR);
    }

    ACPI_FLUSH_CPU_CACHE();

    status = acpi_hw_register_write(ACPI_REGISTER_PM1A_CONTROL, 
                                    acpi_sinfo.pm1a_cnt_val);
    if ( ACPI_FAILURE(status) )
        return_ACPI_STATUS(AE_ERROR);

    if ( acpi_sinfo.pm1b_cnt_blk.address )
    {
        status = acpi_hw_register_write(ACPI_REGISTER_PM1B_CONTROL, 
                                        acpi_sinfo.pm1b_cnt_val);
        if ( ACPI_FAILURE(status) )
            return_ACPI_STATUS(AE_ERROR);
    }

    /* Wait until we enter sleep state, and spin until we wake */
    while ( !acpi_get_wake_status() )
        continue;

    return_ACPI_STATUS(AE_OK);
}

static int __init acpi_sleep_init(void)
{
    int i;
    char *p = opt_acpi_sleep;

    while ( (p != NULL) && (*p != '\0') )
    {
        if ( !strncmp(p, "s3_bios", 7) )
            acpi_video_flags |= 1;
        if ( !strncmp(p, "s3_mode", 7) )
            acpi_video_flags |= 2;
        p = strchr(p, ',');
        if ( p != NULL )
            p += strspn(p, ", \t");
    }

    printk(XENLOG_INFO "<PM> ACPI (supports");
    for ( i = 0; i < ACPI_S_STATE_COUNT; i++ )
    {
        if ( i == ACPI_STATE_S3 )
        {
            sleep_states[i] = 1;
            printk(" S%d", i);
        }
        else
            sleep_states[i] = 0;
    }
    printk(")\n");

    return 0;
}
__initcall(acpi_sleep_init);
