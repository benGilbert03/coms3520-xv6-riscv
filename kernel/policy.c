// Project 1C: scheduling policies.
//
// This is the file you modify. The scheduler framework in proc.c
// calls the functions below; it already handles locking and the
// context switch, so a policy only decides which process runs next
// and when the running process should be preempted.
//
// Each policy has an ID. Select one at run time with the
// set_scheduler(id) system call (for example: wl 2 1). To try a
// different setting, such as another quantum, give it its own ID.
//
// Rules for policy code (Section 2.2 of the project document):
//  1. The system has one CPU. These functions run with interrupts off
//     and must not sleep or acquire p->lock (the framework holds the
//     locks it needs).
//  2. policy_pick_next_proc() must only return a RUNNABLE process, or
//     0 if there is none.
//  3. A scheduler meant to be fair must not starve a job: every ready
//     job eventually runs. (Strict priority, ID 2, is the one
//     deliberate exception.)
//  4. A scheduler must not recognize a workload: do not read
//     p->wl_slot, workload IDs, process names, or pids to decide how
//     to treat a process.
//  5. policy_init(id) may be called more than once for the same ID,
//     including at the start of every workload run. Each call must
//     reset all of the scheduler's global state.
//  6. Do not modify the protected files (kernel/workload.c,
//     kernel/workload.h, user/wl.c).
//
// Scheduler IDs added (Feature 7):
//  3  SCHED_MLFQ  1C MLFQ: 3 queues, start queue from nice, allotment, 60-tick boost
//  6  SCHED_RR5   round robin, 5-tick (50 ms) quantum

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define SCHED_RR     0 // round robin, 10-tick (100 ms) quantum: stock xv6
#define SCHED_RR1    1 // round robin, 1-tick (10 ms) quantum
#define SCHED_PRIO   2 // strict priority by nice (Project 1B), 10-tick quantum
#define SCHED_MLFQ   3 // multi-level feedback queue (Section 3.5)
#define SCHED_RR5    6 // round robin, 5-tick (50 ms) quantum

// Linux CFS weights for nice -20 .. 19, indexed by nice + 20
// (nice 0 has weight 1024). This is the same table user/wl.c uses
// for the fairness metric. For example, a fair-share scheduler can
// advance a process's vrt by (1 << 24) / nice_weight[p->nice + 20]
// for each tick it runs.
const int nice_weight[40] = {
  88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
  9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
  1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
  110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static int quantum = 10; // ticks a process may run before preemption
static int last = -1;    // index in proc[] of the last process chosen

// MLFQ settings, indexed by queue level (0 = Q0 lowest, 2 = Q2 highest).
// Kept in globals set by policy_init() so a tuning variant is a new case.
#define MLFQ_LEVELS 3
static int mlfq_quantum[MLFQ_LEVELS];  // ticks per quantum at each level
static int mlfq_allot[MLFQ_LEVELS];    // allotment at each level (Q0: unused)
static int boost_interval;             // ticks between priority boosts
static uint last_boost;                // value of ticks at the last boost
static int last_at[MLFQ_LEVELS];       // per-level round-robin position

// Called by set_scheduler(id), and again when a workload starts.
// Set up global state for policy id.
// Return 0 if id is a known policy, -1 otherwise.
int
policy_init(int id)
{
  switch (id) {
  case SCHED_RR:
  case SCHED_PRIO:
    quantum = 10;
    break;
  case SCHED_RR1:
    quantum = 1;
    break;
  case SCHED_RR5:
    quantum = 5;
    break;
  case SCHED_MLFQ:
    mlfq_quantum[0] = 15; mlfq_allot[0] = 0; // Q0: no further demotion
    mlfq_quantum[1] = 10; mlfq_allot[1] = 10;
    mlfq_quantum[2] = 2;  mlfq_allot[2] = 2;
    boost_interval = 60;
    break;
  default:
    return -1;
  }
  last = -1;
  for (int i = 0; i < MLFQ_LEVELS; i++)
    last_at[i] = -1;
  last_boost = ticks; // Rule 5: first boost comes boost_interval ticks from now
  return 0;
}

// MLFQ Rule 3: the starting queue for a nice value.
static int
start_level(int nice)
{
  if (nice <= -10)
    return 2;
  if (nice <= 10)
    return 1;
  return 0;
}

// Move p to queue lvl, printing the change while logging is enabled.
// The pid appears only in the log line; it is never used to decide.
static void
set_level(struct proc *p, int lvl)
{
  if (lvl == p->level)
    return;
  if (logging)
    printk("level %d %d->%d at %d\n", p->pid, p->level, lvl, ticks);
  p->level = lvl;
}

// MLFQ Rule 5: every boost_interval ticks, move every process to its
// starting queue or Q1, whichever is higher, and reset its used time
// even if its queue does not change. Does not restart the quantum.
static void
mlfq_boost(void)
{
  if (ticks - last_boost < boost_interval)
    return;
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    set_level(p, p->start > 1 ? p->start : 1);
    p->used = 0;
  }
  last_boost = ticks;
}

// Called when a process is created, and for every process when the
// policy changes or a workload starts. Initialize any per-process
// policy fields.
void
policy_proc_init(struct proc *p)
{
  p->qticks = 0;
  // Level 0 is the lowest queue (Q0); Q2 is the highest. MLFQ Rule 3:
  // the starting level comes from p->nice. workload_start() calls this
  // for every process after the jobs' nice values are set. Policies
  // other than MLFQ ignore level and start.
  p->start = start_level(p->nice);
  p->level = p->start;
  p->used = 0;
  p->vrt = 0;
}

// Round robin: the next RUNNABLE process after the last one chosen,
// scanning proc[] in a circle so no process is starved.
static struct proc *
pick_rr(void)
{
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

// Strict priority: a RUNNABLE process with the lowest nice value;
// processes with equal nice take turns in round-robin order.
static struct proc *
pick_prio(void)
{
  int best = 100;

  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->nice < best)
      best = p->nice;
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE && p->nice == best) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

// MLFQ selection. Rule 5: check for a boost first, so it happens on
// time even while every process sleeps. Rule 1: only the highest level
// with a RUNNABLE process is considered. Rule 2: round robin within
// that level, resuming after the last process chosen at that level;
// one position per level, so a higher-queue job running in between
// cannot move a lower level's position and starve a job there.
static struct proc *
pick_mlfq(void)
{
  int top = -1;

  mlfq_boost();
  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->level > top)
      top = p->level;
  if (top < 0)
    return 0;
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last_at[top] + i) % NPROC];
    if (p->state == RUNNABLE && p->level == top) {
      last_at[top] = p - proc;
      return p;
    }
  }
  return 0;
}

// Choose the next process to run, or return 0 if none is RUNNABLE.
struct proc *
policy_pick_next_proc(void)
{
  switch (sched_current) {
  case SCHED_PRIO:
    return pick_prio();
  case SCHED_MLFQ:
    return pick_mlfq();
  default:
    return pick_rr();
  }
}

// Called just before the framework switches to p.
void
policy_dispatch(struct proc *p)
{
  p->qticks = 0; // a new quantum starts
}

// MLFQ tick, in the order of Section 3.5: charge the tick, check for a
// boost, check the allotment and the quantum, then check Rule 1.
static int
tick_mlfq(struct proc *p)
{
  // Rule 4: used carries across sleeps; it is reset only when the
  // process changes queues (here, in mlfq_boost(), or in policy_nice()).
  p->used++;
  p->qticks++;

  mlfq_boost(); // Rule 5

  // Rule 4: allotment used up -> down one queue. Q0 is never demoted.
  if (p->level > 0 && p->used >= mlfq_allot[p->level]) {
    set_level(p, p->level - 1);
    p->used = 0;
    return 1;
  }

  // Rule 2: quantum of the current queue. A queue change does not
  // restart the quantum; only policy_dispatch() resets qticks.
  if (p->qticks >= mlfq_quantum[p->level])
    return 1;

  // Rule 1: a RUNNABLE process in a higher queue preempts p now.
  for (struct proc *q = proc; q < &proc[NPROC]; q++)
    if (q->state == RUNNABLE && q->level > p->level)
      return 1;
  return 0;
}

// Called on every timer tick while p is running.
// Return 1 to preempt p, or 0 to let it keep running.
// A process is charged only for ticks that arrive while it is running.
// A loop of pause(1) wakes at a tick, runs briefly, and sleeps again
// before the next one, so it is never charged and cannot test a rule
// such as an allotment kept across sleeps. A test that is charged must
// keep the CPU across a tick, for example by spinning until uptime()
// changes before each pause(1); workload jobs are charged correctly.
int
policy_tick(struct proc *p)
{
  if (sched_current == SCHED_MLFQ)
    return tick_mlfq(p);
  p->qticks++;
  return p->qticks >= quantum;
}

// Called when p wakes from sleep (for example, when its simulated I/O
// or a pause() ends) and becomes RUNNABLE again. Use it to decide how
// a waking process re-enters the policy; for example, a fair-share or
// stride policy may move a process that slept a long time up to the
// current minimum, so it cannot claim the CPU for the time it slept.
// MLFQ does nothing here: a waking process keeps its level and used
// time (Rule 4), so sleeping cannot reset its allotment.
void
policy_wakeup(struct proc *p)
{
}

// Called whenever p's nice value changes.
void
policy_nice(struct proc *p)
{
  // MLFQ Rule 6: move to the new starting queue only if it differs,
  // so calling nice() without changing queues cannot reset used.
  int s = start_level(p->nice);

  if (sched_current != SCHED_MLFQ || s == p->start)
    return;
  p->start = s;
  set_level(p, s);
  p->used = 0;
}
