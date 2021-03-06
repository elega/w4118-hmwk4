/* linux/kernel/sched/grr.c
 *
 * Round-Robin Scheduling Class (mapped to the SCHED_GRR policy)
 *
 * Copyright (C) 2014 V. Atlidakis, G. Koloventzos, A. Papancea
 *
 */
#include "sched.h"

#include <linux/slab.h>
#include <linux/limits.h>


#if defined(CONFIG_SMP) && defined(CONFIG_GRR_GROUPS)
/*
 * Helperes to get the cgroup of tasks
 */
static char group_path[PATH_MAX];

static char *task_group_path(struct task_group *tg)
{
	if (autogroup_path(tg, group_path, PATH_MAX))
		return group_path;
	if (!tg->css.cgroup) {
		group_path[0] = '\0';
		return group_path;
	}
	cgroup_path(tg->css.cgroup, group_path, PATH_MAX);
	return group_path;
}

/*
 * Helper to check whether a task can move from a
 * source rq to a target rq.
 */
static int can_move_grr_task(struct task_struct *p,
			     struct rq *source,
			     struct rq *target)
{
	/* see __migrate_task() in core.c for details */
	if (!cpumask_test_cpu(target->cpu, tsk_cpus_allowed(p)))
		return 0;
	if (!cpu_online(target->cpu))
		return 0;
	if (task_cpu(p) != source->cpu)
		return 0;
	if (task_running(source, p))
		return 0;
	return 1;
}
#endif /* CONFIG_GRR_GROUPS && CONFIG_SMP*/


static inline struct task_struct *grr_task_of(struct sched_grr_entity *grr_se)
{
	return container_of(grr_se, struct task_struct, grr);
}

static inline struct list_head *grr_queue_of_rq(struct rq *rq)
{
	return &rq->grr.queue;
}

/*
 * Update current task runtime statistics.
 */
static void update_curr_grr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;

	if (curr->sched_class != &grr_sched_class)
		return;

	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	 curr->se.sum_exec_runtime += delta_exec;
	 account_group_exec_runtime(curr, delta_exec);

	 curr->se.exec_start = rq->clock_task;
	 cpuacct_charge(curr, delta_exec);
}

static void dequeue_grr_entity(struct rq *rq, struct sched_grr_entity *grr_se)
{
	/*
	 * Drop connection of this entity with runque but
	 * reinitialize it to be reconnected later.
	 */
	list_del_init(&grr_se->task_queue);
	--rq->grr.grr_nr_running;
}

static void
enqueue_grr_entity(struct rq *rq, struct sched_grr_entity *grr_se, bool head)
{
	struct list_head *queue = grr_queue_of_rq(rq);

	if (head)
		list_add(&grr_se->task_queue, queue);
	else
		list_add_tail(&grr_se->task_queue, queue);
	++rq->grr.grr_nr_running;
}

static void requeue_task_grr(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_grr_entity *grr_se = &p->grr;
	struct list_head *queue = grr_queue_of_rq(rq);

	if (head)
		list_move(&grr_se->task_queue, queue);
	else
		list_move_tail(&grr_se->task_queue, queue);
}

static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		p->grr.timeout++;
		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->grr.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}
}

/*
 * --------------------------------------------------------------
 *
 * Ok, enough with helpers,
 *
 * Implement the metchods required for grr_sched_class scheduler
 *
 * ---------------------------------------------------------------
 */
void init_grr_rq(struct grr_rq *grr_rq)
{
	INIT_LIST_HEAD(&grr_rq->queue);
	grr_rq->grr_nr_running = 0;

}

static void
enqueue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_grr_entity *grr_se = &(p->grr);

	if (flags & ENQUEUE_WAKEUP)
		grr_se->timeout = 0;

	enqueue_grr_entity(rq, grr_se, flags & ENQUEUE_HEAD);
	inc_nr_running(rq);
}

static void
dequeue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_grr_entity *grr_se = &p->grr;

	update_curr_grr(rq);

	dequeue_grr_entity(rq, grr_se);
	dec_nr_running(rq);
}

static void yield_task_grr(struct rq *rq)
{
	requeue_task_grr(rq, rq->curr, 0);
}

/*
 * grr_sched_class does no preemption
 */
static void
check_preempt_curr_grr(struct rq *rq, struct task_struct *p, int flags)
{
	(void)rq;
	(void)p;
	(void)flags;
}

/*
 * Pick the task on the head of the runqueue (RR).
 */
static struct task_struct *pick_next_task_grr(struct rq *rq)
{
	struct sched_grr_entity *head;
	struct task_struct *p;
	struct grr_rq *grr_rq  = &rq->grr;

	if (unlikely(!grr_rq->grr_nr_running))
		return NULL;

	head = list_first_entry(&rq->grr.queue, struct sched_grr_entity,
				task_queue);
	p = grr_task_of(head);
	if (!p)
		return NULL;

	p->se.exec_start = rq->clock;

	return p;
}

/*
 * Update statistics of a task which is (usually) about to be preempted.
 */
static void put_prev_task_grr(struct rq *rq, struct task_struct *p)
{
	update_curr_grr(rq);

	/*
	 * grr_sched_class uses a RR policy; thus, task should
	 * start from 0.
	 */
	p->se.exec_start = 0;
}

#ifdef CONFIG_SMP

/*
 * Select the runqueue with the least running tasks.
 *
 * Note that in case of unicore we only have one queue.
 */
static int
select_task_rq_grr(struct task_struct *p, int sd_flag, int flags)
{

	struct rq *rq;
	int min_cpu;
	int orig_cpu;
	unsigned long orig_nr;
	unsigned long i;
	unsigned long min_nr;
	int len = 0;
	char *grouppath = task_group_path(task_group(p));
	len = strlen(grouppath);

	orig_cpu = task_cpu(p);
	if (p->grr.nr_cpus_allowed == 1)
		return orig_cpu;

	orig_nr = cpu_rq(orig_cpu)->grr.grr_nr_running;
	min_nr = orig_nr;

	rq = cpu_rq(orig_cpu);
	min_cpu = orig_cpu;

	rcu_read_lock();
	for_each_online_cpu(i) {
		struct grr_rq *grr_rq = &cpu_rq(i)->grr;
#ifdef CONFIG_GRR_GROUPS
		if (len >= 5 && rq->foreground)
			continue;
		if (len < 5 && rq->background)
			continue;
#endif
		if (!cpumask_test_cpu(i, &p->cpus_allowed))
			continue;
		if (grr_rq->grr_nr_running < min_nr) {
			min_nr = grr_rq->grr_nr_running;
			min_cpu = i;
		}
	}
	rcu_read_unlock();

	return min_cpu;
}
#endif

/*
 * OK, this a bit complicated function and is used to move
 * tasks from one group to another.
 *
 * The main complexity emmanates from the fact that there are
 * some conditions need be fulfilled before you move from a
 * queue of one group to a queue of another group. Also, when
 * moving task we try to target the less loaded rq
 * (use select_task_rq).
 *
 * All of the above happens  with local irqs turned off and a
 * double lock held on both source and target rq. Note also
 * the use of Messa Semantics, i.e., rechecking the source rq
 * with the double lock held.
 */
static void task_move_group_grr(struct task_struct *p, int on_rq)
{
	int rq_id;
	unsigned long flags;
	struct rq *target_rq;
	struct rq *source_rq;

	/*
	 * Select the less loaded queue that belong to the
	 * group you want to get attached to.
	 */
	rq_id = select_task_rq_grr(p, 0, 0);
	target_rq = cpu_rq(rq_id);
	source_rq = task_rq(p);

	/*
	 * Start with double locking and save local interrupts
	 * but check a bunch of things before moving indeed.
	 */
	local_irq_save(flags);
	double_rq_lock(source_rq, target_rq);

	/* Now that we hold the lock, are you still in the same rq? */
	if (task_rq(p) != source_rq)
		goto unlock;
	/* But...can you move to the selected queue? */
	if (!can_move_grr_task(p, source_rq, target_rq))
		goto unlock;
	/* And...are you sure that indeed you are moving? */
	if (target_rq == source_rq)
		goto unlock;
	/* Haha...but...are you not blocked? */
	if (p->state != TASK_RUNNING && p->state != TASK_WAKING
	    && !(task_thread_info(p)->preempt_count & PREEMPT_ACTIVE))
		goto unlock;
	/* OK. FINE. Move away....*/
	deactivate_task(source_rq, p, 0);
	set_task_cpu(p, target_rq->cpu);
	activate_task(target_rq, p, 0);

unlock:
	double_rq_unlock(source_rq, target_rq);
	local_irq_restore(flags);
}

/*
 * register when a task started executing.
 */
static void set_curr_task_grr(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq->clock_task;
}

/*
 * used by scheduler_tick to provide the OS
 * with periodic control on any running task
 */
static void task_tick_grr(struct rq *rq, struct task_struct *p, int queued)
{
	struct list_head *queue = grr_queue_of_rq(rq);

	update_curr_grr(rq);

	watchdog(rq, p);

	if (--p->grr.time_slice)
		return;

	p->grr.time_slice = GRR_TIMESLICE;

	/* Requeue if we're not the only task in the queuede */
	if (queue->prev != queue->next) {
		requeue_task_grr(rq, p, 0);
		resched_task(p);
	}
}

/*
 * grr_sched_class has no notion of prio
 */
static void
prio_changed_grr(struct rq *rq, struct task_struct *p, int oldprio)
{
	(void)rq;
	(void)p;
	(void)oldprio;
}

/*
 * When switching a task to GRR, we may overload the
 * runqueue with GRR tasks. In this case we try to
 * push them off to other runqueues.
 */
static void switched_to_grr(struct rq *rq, struct task_struct *p)
{
	if (p->on_rq && rq->curr != p)
		if (rq == task_rq(p) && !rt_task(rq->curr))
			resched_task(rq->curr);
}

static inline unsigned int
get_rr_interval_grr(struct rq *rq, struct task_struct *task)
{
	return GRR_TIMESLICE;
}

/*
 * All the GRR scheduling class methods:
 */
const struct sched_class grr_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_grr,
	.dequeue_task		= dequeue_task_grr,
	.yield_task		= yield_task_grr,
	.check_preempt_curr	= check_preempt_curr_grr,
	.pick_next_task		= pick_next_task_grr,
	.put_prev_task		= put_prev_task_grr,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_grr,
#ifdef CONFIG_GRR_GROUPS
	.task_move_group	= task_move_group_grr,
#endif
#endif
	.set_curr_task          = set_curr_task_grr,
	.task_tick		= task_tick_grr,
	.prio_changed		= prio_changed_grr,
	.switched_to		= switched_to_grr,
	.get_rr_interval	= get_rr_interval_grr,
};


#ifdef CONFIG_SMP

/*
 * Function that tries to...
 */
void try_steal_task(struct rq *target_rq, int dest_cpu)
{
	unsigned long i;
	struct rq *source_rq;
	struct task_struct *p;
	struct sched_grr_entity *grr_se;

	for_each_online_cpu(i) {

		source_rq = cpu_rq(i);

#ifdef CONFIG_GRR_GROUPS
		if (source_rq->background != target_rq->background ||
		    source_rq->foreground != target_rq->foreground)
			continue;
#endif
		if (source_rq == target_rq)
			continue;

		double_rq_lock(source_rq, target_rq);

		if (list_empty(&source_rq->grr.queue)) {
			double_rq_unlock(source_rq, target_rq);
			continue;
		}

		list_for_each_entry(grr_se, &source_rq->grr.queue, task_queue) {
			p = grr_task_of(grr_se);

			if (!can_move_grr_task(p, source_rq, target_rq)
			    || p->policy != SCHED_GRR)
				continue;
			if (p->on_rq) {
				dequeue_task_grr(source_rq, p, 0);
				set_task_cpu(p, dest_cpu);
				enqueue_task_grr(target_rq, p, 0);
				check_preempt_curr(target_rq, p, 0);
				double_rq_unlock(source_rq, target_rq);
				trace_printk("idle CPU %d stole task %s from CPU %d\n",
					     (int) dest_cpu, p->comm, (int) i);
				return;
			}
		}
		double_rq_unlock(source_rq, target_rq);
	}
}

struct load {
	unsigned long nr_running;
	struct rq *rq;
	int cpu;
};

/*
 * Load balancer that does...
 */
void grr_load_balance(void)
{
	unsigned long i;
	int cpus_online, j;
	struct load maxload;
	struct load minload;
	struct rq *source_rq;
	struct rq *target_rq;
	struct task_struct *p;
	unsigned long flags;
	struct sched_grr_entity *grr_se;

	trace_printk("Starting loadbalancing\n");

#ifdef CONFIG_GRR_GROUPS
	for (j = FOREGROUND; j <= BACKGROUND; j++) {
#else
	j = 1;
#endif
	cpus_online = 0;
	maxload.nr_running = 0;
	minload.nr_running = 1000000;

	/*
	 * iterate through each CPU and
	 * find the min and max load accros all CPUs
	 */
	for_each_online_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct grr_rq *grr_rq = &rq->grr;
		unsigned long nr_running = grr_rq->grr_nr_running;

#ifdef CONFIG_GRR_GROUPS
		if (j == FOREGROUND && !rq->foreground)
			continue;
		else if (j == BACKGROUND && !rq->background)
			continue;
#endif
		if (nr_running > maxload.nr_running) {
			maxload.nr_running = nr_running;
			maxload.rq = rq;
			maxload.cpu = i;
		}
		if (nr_running < minload.nr_running) {
			minload.nr_running = nr_running;
			minload.rq = rq;
			minload.cpu = i;
		}
		cpus_online++;
	}
	if (cpus_online < 2)
		return;

	if (maxload.nr_running > minload.nr_running + 1) {

		source_rq = maxload.rq;
		target_rq = minload.rq;
		local_irq_save(flags);
		double_rq_lock(source_rq, target_rq);

		/* imbalance no longer valid */
		if (source_rq->grr.grr_nr_running <=
		    target_rq->grr.grr_nr_running + 1)
			goto unlock;

		list_for_each_entry(grr_se, &source_rq->grr.queue, task_queue) {
			p = grr_task_of(grr_se);
			if (!can_move_grr_task(p, source_rq, target_rq))
				continue;
			/*
			 * move task p from source_rq to target_rq
			 * see sched_move_task() in core.c for details
			 */
			deactivate_task(source_rq, p, 0);
			set_task_cpu(p, target_rq->cpu);
			activate_task(target_rq, p, 0);
			trace_printk("Moved task %s from CPU %d to CPU %d\n",
				     p->comm, source_rq->cpu,
				     target_rq->cpu);
			goto unlock;
		}
		goto unlock;
	}
#ifdef CONFIG_GRR_GROUPS
	trace_printk("Finished loadbalancing of group: %d (no migration)\n", j);
	continue;
#else
	trace_printk("Finished loadbalancing (no migration)\n");
	return;
#endif
unlock:
	double_rq_unlock(source_rq, target_rq);
	local_irq_restore(flags);
#ifdef CONFIG_GRR_GROUPS
		trace_printk("Finished loadbalancing of group: %d\n", j);
	} /* for block end */
#else
	trace_printk("Finished loadbalancing\n");
#endif
}

#endif /* CONFIG_SMP */
