/* include/linux/ckrm_sched.h - Supports CKRM scheduling
 *
 * Copyright (C) Haoqiang Zheng,  IBM Corp. 2004
 * Copyright (C) Hubertus Franke,  IBM Corp. 2004
 * 
 * Latest version, more details at http://ckrm.sf.net
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef _CKRM_SCHED_H
#define _CKRM_SCHED_H

#include <linux/sched.h>
#include <linux/ckrm_rc.h>
#include <linux/ckrm_classqueue.h>
#include <linux/random.h>

#define BITMAP_SIZE ((((MAX_PRIO+1+7)/8)+sizeof(long)-1)/sizeof(long))

struct prio_array {
	unsigned int nr_active;
	unsigned long bitmap[BITMAP_SIZE];
	struct list_head queue[MAX_PRIO];
};

#ifdef CONFIG_CKRM_CPU_SCHEDULE
#define rq_active(p,rq)   (get_task_lrq(p)->active)
#define rq_expired(p,rq)  (get_task_lrq(p)->expired)
int __init init_ckrm_sched_res(void);
#else
#define rq_active(p,rq)   (rq->active)
#define rq_expired(p,rq)  (rq->expired)
static inline void init_ckrm_sched_res(void) {}
static inline int ckrm_cpu_monitor_init(void) {return 0;}
#endif

#ifdef CONFIG_CKRM_CPU_SCHEDULE
struct ckrm_runqueue {
	cq_node_t classqueue_linkobj;	/*links in classqueue */
	struct ckrm_cpu_class *cpu_class;	// class it belongs to
	struct classqueue_struct *classqueue;	// classqueue it belongs tow
	unsigned long long uncounted_ns;

	prio_array_t *active, *expired, arrays[2];
	/*
	   set to 0 on init, become null or array switch
	   set to jiffies whenever an non-interactive job expires
	   reset to jiffies if expires
	 */
	unsigned long expired_timestamp;

	/* 
	 * highest priority of tasks in active
	 * initialized to be MAX_PRIO
	 * updated on enqueue, dequeue
	 */
	int top_priority;
	CVT_t local_cvt;

	unsigned long lrq_load;
	int local_weight; 

	unsigned long magic;	//for debugging
};

typedef struct ckrm_runqueue ckrm_lrq_t;

/**
 * ckrm_cpu_class_stat - cpu usage statistics maintained for each class
 * 
 */
struct ckrm_cpu_class_stat {
	spinlock_t stat_lock;

	unsigned long long total_ns;	/*how much nano-secs it has consumed */

	struct ckrm_cpu_demand_stat local_stats[NR_CPUS];

	/* 
	 * 
	 */
	unsigned long max_demand; /* the maximun a class can consume */
	int egrt,megrt; /*effective guarantee*/
	int ehl,mehl; /*effective hard limit, my effective hard limit*/

	/*
	 * eshare: for both default class and its children
	 * meshare: just for the default class
	 */
	int eshare;
	int meshare;
};

#define CKRM_CPU_CLASS_MAGIC 0x7af2abe3
/*
 * manages the class status
 * there should be only one instance of this object for each class in the whole system  
 */
struct ckrm_cpu_class {
	struct ckrm_core_class *core;
	struct ckrm_core_class *parent;
	struct ckrm_shares shares;
	spinlock_t cnt_lock;	// always grab parent's lock first and then child's
	struct ckrm_cpu_class_stat stat;
	struct list_head links;	// for linking up in cpu classes
	ckrm_lrq_t local_queues[NR_CPUS];	// runqueues 
	unsigned long magic;	//for debugging
};

#define cpu_class_weight(cls) (cls->stat.meshare)
#define local_class_weight(lrq) (lrq->local_weight)

static inline int valid_cpu_class(struct ckrm_cpu_class * cls)
{
	return (cls && cls->magic == CKRM_CPU_CLASS_MAGIC);
}

struct classqueue_struct *get_cpu_classqueue(int cpu);
struct ckrm_cpu_class * get_default_cpu_class(void);

#define lrq_nr_running(lrq) \
             (lrq->active->nr_active + lrq->expired->nr_active)

static inline ckrm_lrq_t *
get_ckrm_lrq(struct ckrm_cpu_class*cls, int cpu)
{
	return &(cls->local_queues[cpu]);
}

static inline ckrm_lrq_t *get_task_lrq(struct task_struct *p)
{
	return &(p->cpu_class->local_queues[task_cpu(p)]);
}

#define task_list_entry(list)  list_entry(list,struct task_struct,run_list)
#define class_list_entry(list) list_entry(list,struct ckrm_runqueue,classqueue_linkobj)

/* some additional interfaces exported from sched.c */
struct runqueue;
extern rwlock_t class_list_lock;
extern struct list_head active_cpu_classes;
unsigned int task_timeslice(task_t *p);
void _ckrm_cpu_change_class(task_t *task, struct ckrm_cpu_class *newcls);

void init_cpu_classes(void);
void init_cpu_class(struct ckrm_cpu_class *cls,ckrm_shares_t* shares);
void ckrm_cpu_change_class(void *task, void *old, void *new);


#define CPU_DEMAND_ENQUEUE 0
#define CPU_DEMAND_DEQUEUE 1
#define CPU_DEMAND_DESCHEDULE 2
#define CPU_DEMAND_INIT 3

/*functions exported by ckrm_cpu_monitor.c*/
void ckrm_cpu_monitor(void);
int ckrm_cpu_monitor_init(void);
void ckrm_cpu_stat_init(struct ckrm_cpu_class_stat *stat);
void cpu_demand_event(struct ckrm_cpu_demand_stat* local_stat, int event, unsigned long long len);
void adjust_local_weight(void);

#define get_task_lrq_stat(p) (&(p)->cpu_class->stat.local_stats[task_cpu(p)])
#define get_cls_local_stat(cls,cpu) (&(cls)->stat.local_stats[cpu])
#define get_rq_local_stat(lrq,cpu) (get_cls_local_stat((lrq)->cpu_class,cpu))

/**
 * get_effective_prio: return the effective priority of a class local queue
 *
 * class priority = progress * a + urgency * b
 * progress = queue cvt
 * urgency = queue top priority
 * a and b are scaling factors  
 * currently, prio increases by 1 if either: top_priority increase by one
 *                                   or, local_cvt increases by 4ms
 */
#define CLASS_QUANTIZER 22	//shift from ns to increase class bonus
#define PRIORITY_QUANTIZER 0	//controls how much a high prio task can borrow
#define CVT_INTERACTIVE_BONUS ((CLASSQUEUE_SIZE << CLASS_QUANTIZER)*2)
static inline int get_effective_prio(ckrm_lrq_t * lrq)
{
	int prio;

	prio = lrq->local_cvt >> CLASS_QUANTIZER;  // cumulative usage
	prio += lrq->top_priority >> PRIORITY_QUANTIZER; // queue urgency

	return prio;
}

/** 
 * update_class_priority:
 * 
 * called whenever cvt or top_priority changes
 *
 * internal: (calling structure)
 * update_class_priority
 *   -- set_top_priority
 *      -- class_enqueue_task
 *      -- class_dequeue_task
 *      -- rq_get_next_task (queue switch)
 *   -- update_local_cvt
 *      -- schedule
 */
static inline void update_class_priority(ckrm_lrq_t *local_rq)
{
	int effective_prio = get_effective_prio(local_rq);
	classqueue_update_prio(local_rq->classqueue,
			       &local_rq->classqueue_linkobj,
			       effective_prio);
}

/*
 *  set the new top priority and reposition the queue
 *  called when: task enqueue/dequeue and queue switch
 */
static inline void set_top_priority(ckrm_lrq_t *lrq,
				    int new_priority)
{
	lrq->top_priority = new_priority;
	update_class_priority(lrq);
}

/*
 * task_load: how much load this task counts
 */
static inline unsigned long task_load(struct task_struct* p)
{
	return (task_timeslice(p) * p->demand_stat.cpu_demand);
}

/*
 * runqueue load is the local_weight of all the classes on this cpu
 * must be called with class_list_lock held
 */
static inline unsigned long ckrm_cpu_load(int cpu)
{
	struct ckrm_cpu_class *clsptr;
	ckrm_lrq_t* lrq;
	struct ckrm_cpu_demand_stat* l_stat;
	int total_load = 0;
	int load;

	list_for_each_entry(clsptr,&active_cpu_classes,links) {
		lrq =  get_ckrm_lrq(clsptr,cpu);
		l_stat = get_cls_local_stat(clsptr,cpu);
		load = lrq->local_weight;
		if (l_stat->cpu_demand < load)
			load = l_stat->cpu_demand;
		total_load += load;
	}	
	return total_load;
}

static inline void class_enqueue_task(struct task_struct *p,
				      prio_array_t * array)
{
	ckrm_lrq_t *lrq;
	int effective_prio;


	lrq = get_task_lrq(p);

	cpu_demand_event(&p->demand_stat,CPU_DEMAND_ENQUEUE,0);
	lrq->lrq_load += task_load(p);

	if ((p->prio < lrq->top_priority) && (array == lrq->active))
		set_top_priority(lrq, p->prio);	

	if (! cls_in_classqueue(&lrq->classqueue_linkobj)) {
		cpu_demand_event(get_task_lrq_stat(p),CPU_DEMAND_ENQUEUE,0);
		effective_prio = get_effective_prio(lrq);
		classqueue_enqueue(lrq->classqueue, &lrq->classqueue_linkobj, effective_prio);
	} 

}

static inline void class_dequeue_task(struct task_struct *p,
				      prio_array_t * array)
{
	ckrm_lrq_t *lrq = get_task_lrq(p);
	unsigned long load = task_load(p);
	
	BUG_ON(lrq->lrq_load < load);
	lrq->lrq_load -= load;

	cpu_demand_event(&p->demand_stat,CPU_DEMAND_DEQUEUE,0);

	if ((array == lrq->active) && (p->prio == lrq->top_priority)
	    && list_empty(&(array->queue[p->prio])))
		set_top_priority(lrq,
				 find_next_bit(array->bitmap, MAX_PRIO,
					       p->prio));
}

/*
 *  called after a task is switched out. Update the local cvt accounting 
 *  we need to stick with long instead of long long due to nonexistent 64-bit division
 */
static inline void update_local_cvt(struct task_struct *p, unsigned long nsec)
{
	ckrm_lrq_t * lrq = get_task_lrq(p);

	unsigned long cvt_inc = nsec / local_class_weight(lrq);

	lrq->local_cvt += cvt_inc;
	lrq->uncounted_ns += nsec;

	update_class_priority(lrq);
}

static inline int class_preempts_curr(struct task_struct * p, struct task_struct* curr)
{
	struct cq_node_struct* node1 = &(get_task_lrq(p)->classqueue_linkobj);
	struct cq_node_struct* node2 = &(get_task_lrq(curr)->classqueue_linkobj);

	return (class_compare_prio(node1,node2) < 0);
}

/*
 * return a random value with range [0, (val-1)]
 */
static inline int get_ckrm_rand(unsigned long val)
{
	int rand;

	if (! val)
		return 0;

	get_random_bytes(&rand,sizeof(rand));
	return (rand % val);
}

void update_class_cputime(int this_cpu);

/**********************************************/
/*          PID_LOAD_BALANCING                */
/**********************************************/
struct ckrm_load_struct {
	unsigned long load_p; 	/*propotional*/
	unsigned long load_i;   /*integral   */
	long load_d;   /*derivative */
};

typedef struct ckrm_load_struct ckrm_load_t;

static inline void ckrm_load_init(ckrm_load_t* ckrm_load) {
	ckrm_load->load_p = 0;
	ckrm_load->load_i = 0;
	ckrm_load->load_d = 0;
}

void ckrm_load_sample(ckrm_load_t* ckrm_load,int cpu);
long pid_get_pressure(ckrm_load_t* ckrm_load, int local_group);
#define rq_ckrm_load(rq) (&((rq)->ckrm_load))

static inline void ckrm_sched_tick(int j,int this_cpu,struct ckrm_load_struct* ckrm_load)
{
#define CVT_UPDATE_TICK     ((HZ/2)?:1)
#define CKRM_BASE_UPDATE_RATE 400

	read_lock(&class_list_lock);

#ifdef CONFIG_SMP
	ckrm_load_sample(ckrm_load,this_cpu);
#endif

	if (!(j % CVT_UPDATE_TICK))
		update_class_cputime(this_cpu);

	if (! (j % CKRM_BASE_UPDATE_RATE))
		classqueue_update_base(get_cpu_classqueue(this_cpu));

	read_unlock(&class_list_lock);
}

#endif /*CONFIG_CKRM_CPU_SCHEDULE */

#endif
