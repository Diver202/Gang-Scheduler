/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Gang scheduler.
 *
 * Gang threads are listed in the gang_tids map (thread id -> gang, index).
 * Each gang thread always runs on its home CPU (index % nr_cpus) from its own
 * per-gang, per-CPU queue. Everything else shares one weighted-vtime queue.
 *
 * Time is split into windows that apply to ALL CPUs at once:
 *   gang 0 | gang 1 | ... | others | gang 0 | ...
 * In a gang's window, its threads run first on every CPU. In the others window,
 * background tasks run first. A timer switches windows and kicks every CPU so
 * the switch happens everywhere together. The others window is skipped while
 * no background task is waiting. CPUs never sit idle while anything is runnable.
 */
#include <scx/common.bpf.h>
#include "gang_common.h"

char _license[] SEC("license") = "GPL";

const volatile u64 gang_window_ns = 8000000;
const volatile u64 other_window_ns = 2000000;
const volatile u32 nr_gangs = 1;
const volatile u32 nr_cpus = 1;

/* Current window: 0 .. nr_gangs-1 is a gang's window, nr_gangs is the others window. */
u32 window;
u64 nr_window_switches;

UEI_DEFINE(uei);

#define SHARED_DSQ 0
#define GANG_DSQ(gang, cpu) (1 + (u64)(gang) * nr_cpus + (cpu))

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, u32);
	__type(value, struct gang_member);
} gang_tids SEC(".maps");

enum { STAT_GANG_ENQ, STAT_OTHER_ENQ, NR_GANG_STATS };

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, NR_GANG_STATS);
	__type(key, u32);
	__type(value, u64);
} stats SEC(".maps");

struct window_timer {
	struct bpf_timer timer;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct window_timer);
} window_timer SEC(".maps");

static u64 vtime_now;

static void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);
	if (cnt)
		(*cnt)++;
}

/* Returns the task's gang entry, or NULL if it is a background task. */
static struct gang_member *gang_of(struct task_struct *p)
{
	u32 tid = p->pid;
	struct gang_member *m = bpf_map_lookup_elem(&gang_tids, &tid);

	if (!m || m->gang >= nr_gangs)
		return NULL;
	if (!bpf_cpumask_test_cpu(m->index % nr_cpus, p->cpus_ptr))
		return NULL;
	return m;
}

s32 BPF_STRUCT_OPS(gang_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	struct gang_member *m = gang_of(p);
	bool is_idle = false;

	if (m)
		return m->index % nr_cpus;
	return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
}

void BPF_STRUCT_OPS(gang_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct gang_member *m = gang_of(p);

	if (m) {
		s32 cpu = m->index % nr_cpus;

		stat_inc(STAT_GANG_ENQ);
		scx_bpf_dsq_insert(p, GANG_DSQ(m->gang, cpu), gang_window_ns, enq_flags);
		/* In its own window a gang thread takes its CPU right away. */
		scx_bpf_kick_cpu(cpu, window == m->gang ? SCX_KICK_PREEMPT : SCX_KICK_IDLE);
		return;
	}

	stat_inc(STAT_OTHER_ENQ);
	u64 vtime = p->scx.dsq_vtime;
	if (time_before(vtime, vtime_now - SCX_SLICE_DFL))
		vtime = vtime_now - SCX_SLICE_DFL;
	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, vtime, enq_flags);
}

void BPF_STRUCT_OPS(gang_dispatch, s32 cpu, struct task_struct *prev)
{
	u32 g;

	if (window < nr_gangs && scx_bpf_dsq_move_to_local(GANG_DSQ(window, cpu)))
		return;
	if (scx_bpf_dsq_move_to_local(SHARED_DSQ))
		return;
	bpf_for(g, 0, nr_gangs) {
		if (scx_bpf_dsq_move_to_local(GANG_DSQ(g, cpu)))
			return;
	}
}

void BPF_STRUCT_OPS(gang_running, struct task_struct *p)
{
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
}

void BPF_STRUCT_OPS(gang_stopping, struct task_struct *p, bool runnable)
{
	if (gang_of(p))
		return;
	p->scx.dsq_vtime += (SCX_SLICE_DFL - p->scx.slice) * 100 / p->scx.weight;
}

void BPF_STRUCT_OPS(gang_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = vtime_now;
}

void BPF_STRUCT_OPS(gang_exit_task, struct task_struct *p, struct scx_exit_task_args *args)
{
	u32 tid = p->pid;

	bpf_map_delete_elem(&gang_tids, &tid);
}

static int window_timerfn(void *map, int *key, struct bpf_timer *timer)
{
	u32 next = (window + 1) % (nr_gangs + 1);
	s32 cpu;

	if (next == nr_gangs && scx_bpf_dsq_nr_queued(SHARED_DSQ) == 0)
		next = 0;

	if (next != window) {
		window = next;
		nr_window_switches++;
		bpf_for(cpu, 0, nr_cpus)
			scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
	}

	bpf_timer_start(timer, next == nr_gangs ? other_window_ns : gang_window_ns, 0);
	return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(gang_init)
{
	u32 key = 0, g, cpu;
	struct bpf_timer *timer;
	s32 ret;

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret)
		return ret;

	bpf_for(g, 0, nr_gangs) {
		bpf_for(cpu, 0, nr_cpus) {
			ret = scx_bpf_create_dsq(GANG_DSQ(g, cpu), -1);
			if (ret)
				return ret;
		}
	}

	timer = bpf_map_lookup_elem(&window_timer, &key);
	if (!timer)
		return -ESRCH;
	bpf_timer_init(timer, &window_timer, CLOCK_MONOTONIC);
	bpf_timer_set_callback(timer, window_timerfn);
	return bpf_timer_start(timer, gang_window_ns, 0);
}

void BPF_STRUCT_OPS(gang_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(gang_ops,
	       .select_cpu	= (void *)gang_select_cpu,
	       .enqueue		= (void *)gang_enqueue,
	       .dispatch	= (void *)gang_dispatch,
	       .running		= (void *)gang_running,
	       .stopping	= (void *)gang_stopping,
	       .enable		= (void *)gang_enable,
	       .exit_task	= (void *)gang_exit_task,
	       .init		= (void *)gang_init,
	       .exit		= (void *)gang_exit,
	       .name		= "gang");
