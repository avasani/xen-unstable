#ifndef _X86_64_CURRENT_H
#define _X86_64_CURRENT_H

#if !defined(__ASSEMBLY__)
struct task_struct;

#include <asm/pda.h>

#define STACK_RESERVED \
    (sizeof(execution_context_t))

static inline struct task_struct * get_current(void)
{
    struct task_struct *current;
    current = read_pda(pcurrent);
    return current;
}
 
#define current get_current()

static inline void set_current(struct task_struct *p)
{
    write_pda(pcurrent,p);
}

static inline execution_context_t *get_execution_context(void)
{
    execution_context_t *execution_context;
    __asm__( "andq %%rsp,%0; addq %2,%0"
	    : "=r" (execution_context)
	    : "0" (~(STACK_SIZE-1)), "i" (STACK_SIZE-STACK_RESERVED) ); 
    return execution_context;
}

static inline unsigned long get_stack_top(void)
{
    unsigned long p;
    __asm__ ( "orq %%rsp,%0; andq $~7,%0" 
              : "=r" (p) : "0" (STACK_SIZE-8) );
    return p;
}

#define schedule_tail(_p)                                         \
    __asm__ __volatile__ (                                        \
        "andq %%rsp,%0; addq %2,%0; movq %0,%%rsp; jmp *%1"       \
        : : "r" (~(STACK_SIZE-1)),                                \
            "r" (unlikely(is_idle_task((_p))) ?                   \
                                continue_cpu_idle_loop :          \
                                continue_nonidle_task),           \
            "i" (STACK_SIZE-STACK_RESERVED) )


#else

#ifndef ASM_OFFSET_H
#include <asm/offset.h> 
#endif

#define GET_CURRENT(reg) movq %gs:(pda_pcurrent),reg

#endif

#endif /* !(_X86_64_CURRENT_H) */
