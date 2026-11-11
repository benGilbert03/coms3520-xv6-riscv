// Project 1C: MLFQ rule test for rules no workload shows.
//
// Run under scheduler 3 with logging on. Forks three CPU-bound and two
// I/O-bound children; all start at nice 0, so all start in Q1. The
// parent then raises itself to Q2, so that when it wakes from pause()
// it preempts the children at the next tick (Rule 1) and changes the
// CPU-bound children's nice values while they are still partway
// through their Q1 allotment.
//
// Rule 6 (CPU-bound children):
//   parent:  nice -10 -> "level P 1->2" right after the forks
//   child A: nice -15 -> starting queue Q2: "level A 1->2" at the
//            nice() call
//   child B: nice   3 -> starting queue still Q1: no level line, and
//            its used time is not reset, so "level B 1->0" comes once
//            its Q1 total reaches 10 ticks, not 10 ticks after nice()
//   child C: nice  12 -> starting queue Q0: "level C 1->0" at the
//            nice() call
//
// Rule 4 across sleeps (I/O-bound children D and E): each burst keeps
// the CPU across one tick, so it is charged one tick, then sleeps with
// pause(1). Sleeping does not reset used, so each shows "level 1->0"
// after about 10 bursts, even though it gave up the CPU every time.
//
// The "level P a->b at T" and "nice set to N for P" log lines show
// each move. If children are still running 60 ticks after
// set_scheduler(3), the Rule 5 boost also adds "level P 0->..." lines.

#include "kernel/types.h"
#include "user/user.h"

#define NCPU   3
#define NIO    2
#define SPIN   50 // ticks of elapsed time each CPU-bound child keeps the CPU busy
#define BURSTS 15 // bursts each I/O-bound child runs
#define WAIT   15 // ticks the parent sleeps before changing nice values

// CPU-bound: use the CPU until SPIN ticks have elapsed. A child that
// spins across ticks is charged for them, unlike a pause(1) loop.
static void
cpu_child(void)
{
  int end = uptime() + SPIN;
  volatile int x = 0;

  while (uptime() < end)
    x++;
}

// I/O-bound: each burst spins until uptime() changes, so the child
// holds the CPU across a tick and is charged for it, then sleeps for
// a tick of simulated I/O.
static void
io_child(void)
{
  volatile int x = 0;

  for (int i = 0; i < BURSTS; i++) {
    int t = uptime();
    while (uptime() == t)
      x++;
    pause(1);
  }
}

int
main(void)
{
  int cpu[NCPU], io[NIO];
  int inc[NCPU] = { -15, 3, 12 };

  if (set_scheduler(3) < 0) {
    printf("mlfqtest: set_scheduler(3) failed\n");
    exit(1);
  }
  startLogging();

  // Fork before the parent does any CPU work of its own.
  for (int i = 0; i < NCPU + NIO; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("mlfqtest: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      if (i < NCPU)
        cpu_child();
      else
        io_child();
      exit(0);
    }
    if (i < NCPU)
      cpu[i] = pid;
    else
      io[i - NCPU] = pid;
  }

  // Move the parent to Q2. pause() wakes it at every tick, and each
  // wakeup preempts the running Q1 child (Rule 1), so the children
  // take turns and each has used a few ticks of its allotment when
  // the parent wakes for good. The parent sleeps again before the
  // next tick, so it is never charged and stays in Q2.
  nice(getpid(), -10);
  pause(WAIT);
  for (int i = 0; i < NCPU; i++)
    nice(cpu[i], inc[i]);

  for (int i = 0; i < NCPU + NIO; i++)
    wait(0);
  stopLogging();

  printf("mlfqtest: parent=%d cpu A=%d B=%d C=%d (nice %d %d %d), io D=%d E=%d\n",
         getpid(), cpu[0], cpu[1], cpu[2], inc[0], inc[1], inc[2],
         io[0], io[1]);
  exit(0);
}
