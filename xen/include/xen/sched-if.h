/******************************************************************************
 * Additional declarations for the generic scheduler interface.  This should
 * only be included by files that implement conforming schedulers.
 *
 * Portions by Mark Williamson are (C) 2004 Intel Research Cambridge
 */

#ifndef __XEN_SCHED_IF_H__
#define __XEN_SCHED_IF_H__

//#define ADV_SCHED_HISTO
#define BUCKETS  10
/*300*/

struct schedule_data {
    spinlock_t          schedule_lock;  /* spinlock protecting curr        */
    struct exec_domain *curr;           /* current task                    */
    struct exec_domain *idle;           /* idle task for this cpu          */
    void               *sched_priv;
    struct ac_timer     s_timer;        /* scheduling timer                */
    unsigned long       tick;           /* current periodic 'tick'         */
#ifdef ADV_SCHED_HISTO
    u32			to_hist[BUCKETS];
    u32			from_hist[BUCKETS];
    u64			save_tsc;
#endif
#ifdef BUCKETS
    u32                 hist[BUCKETS];  /* for scheduler latency histogram */
#endif
} __cacheline_aligned;

struct task_slice {
    struct exec_domain *task;
    s_time_t            time;
};

struct scheduler {
    char *name;             /* full name for this scheduler      */
    char *opt_name;         /* option name for this scheduler    */
    unsigned int sched_id;  /* ID for this scheduler             */

    int          (*init_scheduler) (void);
    int          (*init_idle_task) (struct exec_domain *);
    int          (*alloc_task)     (struct exec_domain *);
    void         (*add_task)       (struct exec_domain *);
    void         (*free_task)      (struct domain *);
    void         (*rem_task)       (struct exec_domain *);
    void         (*sleep)          (struct exec_domain *);
    void         (*wake)           (struct exec_domain *);
    struct task_slice (*do_schedule) (s_time_t);
    int          (*control)        (struct sched_ctl_cmd *);
    int          (*adjdom)         (struct domain *,
                                    struct sched_adjdom_cmd *);
    void         (*dump_settings)  (void);
    void         (*dump_cpu_state) (int);
};

extern struct schedule_data schedule_data[];

#endif /* __XEN_SCHED_IF_H__ */
