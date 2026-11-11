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
//  4  SCHED_STRIDE stride scheduling, tickets = nice weight, 10-tick quantum
//  5  SCHED_RSTRIDE responsive stride: ID 12 + dynamic slice, wakeup preemption,
//                  short slice for woken jobs, one-time head start for new jobs
//  6  SCHED_RR5   round robin, 5-tick (50 ms) quantum
// Stride tuning variants (Checkpoint 4), each one change from ID 4:
// 10  SCHED_STRIDE_Q2      stride with a 2-tick quantum (H1)
// 11  SCHED_STRIDE_PREEMPT stride, preempt when a RUNNABLE job has a lower pass (H2)
// 12  SCHED_STRIDE_CREDIT  stride, waking job placed one nice-0 tick below the min pass (H3)
// 13  SCHED_STRIDE_HALF    stride, tickets from nice/2 (compressed weights) (H4)
// 14  SCHED_STRIDE_SCALEQ  stride, quantum scaled by weight, 1..20 ticks (H5)

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
#define SCHED_STRIDE 4 // stride scheduling, tickets = nice weight (Section 3.6)
#define SCHED_RSTRIDE 5 // responsive stride, own design (Section 3.8)
#define SCHED_RR5    6 // round robin, 5-tick (50 ms) quantum
#define SCHED_STRIDE_Q2      10 // H1: stride, 2-tick quantum
#define SCHED_STRIDE_PREEMPT 11 // H2: stride, preempt on a lower pass
#define SCHED_STRIDE_CREDIT  12 // H3: stride, bounded credit for sleepers
#define SCHED_STRIDE_HALF    13 // H4: stride, tickets from nice/2
#define SCHED_STRIDE_SCALEQ  14 // H5: stride, quantum scaled by weight

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

// Stride: a process's tickets are its nice weight, and its stride is
// STRIDE1 / tickets, scaled so every nice value gets a nonzero stride
// (189 at nice -20, 16384 at nice 0, about 1.1M at nice 19). p->vrt
// holds the pass value.
#define STRIDE1 (1 << 24)

// Stride settings, set by policy_init() so each tuning variant is just
// a new case there. ID 4 uses the defaults from stride_defaults().
static int stride_preempt;    // H2: preempt when a RUNNABLE job has a lower pass
static uint64 wake_credit;   // H3: pass a waking job may sit below the min
static int nice_half;        // H4: tickets from nice/2 instead of nice
static int scale_quantum;    // H5: quantum scaled by the job's weight

// Responsive stride (ID 5) settings. Times in ticks; credits and the
// wakeup granularity in pass units (STRIDE1 / 1024 = one nice-0 tick).
static int sched_latency;    // slice = sched_latency / ready jobs (CFS)
static int min_gran;         // shortest slice; also a woken job's slice
static int max_slice;        // longest slice
static uint64 wake_gran;     // a woken job preempts only if this much lower
static uint64 new_credit;    // one-time head start on a job's first arrival

static void
stride_defaults(void)
{
  quantum = 10;
  stride_preempt = 0;
  wake_credit = 0;
  nice_half = 0;
  scale_quantum = 0;
  new_credit = 0;
}

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
  case SCHED_STRIDE:
    stride_defaults();
    break;
  case SCHED_RSTRIDE:
    stride_defaults();
    wake_credit = STRIDE1 / 1024;     // ID 12's sleeper credit
    sched_latency = 20;
    min_gran = 2;
    max_slice = 10;
    wake_gran = 2 * (STRIDE1 / 1024);
    new_credit = 10 * (STRIDE1 / 1024);
    break;
  case SCHED_STRIDE_Q2:
    stride_defaults();
    quantum = 2;
    break;
  case SCHED_STRIDE_PREEMPT:
    stride_defaults();
    stride_preempt = 1;
    break;
  case SCHED_STRIDE_CREDIT:
    stride_defaults();
    wake_credit = STRIDE1 / 1024; // one tick of pass at nice 0
    break;
  case SCHED_STRIDE_HALF:
    stride_defaults();
    nice_half = 1;
    break;
  case SCHED_STRIDE_SCALEQ:
    stride_defaults();
    scale_quantum = 1;
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
  p->woke = 0;
  p->ran = 0;
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

// Is policy id the stride scheduler or one of its tuning variants?
static int
is_stride(int id)
{
  return id == SCHED_STRIDE || id == SCHED_RSTRIDE ||
         (id >= SCHED_STRIDE_Q2 && id <= SCHED_STRIDE_SCALEQ);
}

// Stride: p's tickets, its nice weight. H4 halves the nice value first
// (C division rounds toward 0), which compresses the weight ratios.
static int
stride_weight(struct proc *p)
{
  int n = nice_half ? p->nice / 2 : p->nice;
  return nice_weight[n + 20];
}

// Stride: the pass added for each tick p runs. Computed from the
// current nice value, so a nice change takes effect at the next tick.
static uint64
stride_of(struct proc *p)
{
  return STRIDE1 / stride_weight(p);
}

// Stride: p's quantum. H5 scales it by weight, so each full quantum
// adds about the same pass: 10 ticks at nice 0, clamped to 1..20.
static int
stride_quantum(struct proc *p)
{
  if (!scale_quantum)
    return quantum;
  int q = 10 * stride_weight(p) / 1024;
  if (q < 1)
    q = 1;
  if (q > 20)
    q = 20;
  return q;
}

// Stride selection: the RUNNABLE process with the lowest pass. The scan
// starts after the last process chosen and only a strictly lower pass
// replaces the best so far, so equal passes take turns.
static struct proc *
pick_stride(void)
{
  struct proc *best = 0;

  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE && (best == 0 || p->vrt < best->vrt))
      best = p;
  }
  if (best)
    last = best - proc;
  return best;
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
    if (is_stride(sched_current))
      return pick_stride();
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

// Stride tick: the pass grows by the stride for each tick run. Preempt
// at the end of the quantum, or (H2) as soon as a RUNNABLE job has a
// lower pass than p.
static int
tick_stride(struct proc *p)
{
  p->vrt += stride_of(p);
  p->qticks++;
  if (p->qticks >= stride_quantum(p))
    return 1;
  if (stride_preempt)
    for (struct proc *q = proc; q < &proc[NPROC]; q++)
      if (q->state == RUNNABLE && q->vrt < p->vrt)
        return 1;
  return 0;
}

// Responsive stride (ID 5): the slice is sched_latency divided by the
// number of RUNNING and RUNNABLE processes, clamped to
// [min_gran, max_slice], so few ready jobs get long slices (few
// switches) and many still each get a turn soon. A job that just woke
// gets only min_gran, so it gets on quickly but cannot hold the CPU.
static int
rstride_slice(struct proc *p)
{
  if (p->woke)
    return min_gran;
  int n = 0;
  for (struct proc *q = proc; q < &proc[NPROC]; q++)
    if (q->state == RUNNING || q->state == RUNNABLE)
      n++;
  int s = sched_latency / (n > 0 ? n : 1);
  if (s < min_gran)
    s = min_gran;
  if (s > max_slice)
    s = max_slice;
  return s;
}

// Responsive stride tick: charge the tick to the pass, end the slice
// when it is used up, and otherwise preempt only for a job that just
// woke with a pass more than wake_gran below p's (CFS wakeup
// granularity), so equal batch jobs never preempt each other.
static int
tick_rstride(struct proc *p)
{
  p->vrt += stride_of(p);
  p->qticks++;
  p->ran++;
  if (p->qticks >= rstride_slice(p)) {
    p->woke = 0;
    return 1;
  }
  for (struct proc *q = proc; q < &proc[NPROC]; q++)
    if (q->state == RUNNABLE && q->woke && q->vrt + wake_gran < p->vrt)
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
  if (sched_current == SCHED_RSTRIDE)
    return tick_rstride(p);
  if (is_stride(sched_current))
    return tick_stride(p);
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
// Stride: a waking (or arriving) process's pass is raised to the lowest
// pass of the other RUNNING and RUNNABLE processes, so it cannot use
// the time it slept to take over the CPU. It never moves backward, and
// if no other process is ready it keeps its own pass. A process made
// by fork() becomes RUNNABLE without coming through here, so it starts
// at pass 0; workload jobs always arrive through here. H3 lets a waking
// job sit up to wake_credit below the minimum, a bounded bonus for
// sleeping that cannot be saved up. ID 5 also marks the job as woken,
// and on its first arrival (ran == 0) gives it new_credit instead, a
// one-time head start so short new jobs can finish early.
void
policy_wakeup(struct proc *p)
{
  if (!is_stride(sched_current))
    return;
  p->woke = 1;

  struct proc *min = 0;
  for (struct proc *q = proc; q < &proc[NPROC]; q++) {
    if (q == p || (q->state != RUNNING && q->state != RUNNABLE))
      continue;
    if (min == 0 || q->vrt < min->vrt)
      min = q;
  }
  if (min == 0)
    return;
  uint64 credit = (p->ran == 0 && new_credit) ? new_credit : wake_credit;
  uint64 floor = min->vrt > credit ? min->vrt - credit : 0;
  if (floor > p->vrt)
    p->vrt = floor;
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
