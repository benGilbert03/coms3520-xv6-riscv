// Project 1C: MLFQ Rule 6 test.
//
// Run under scheduler 3 with logging on. Forks three CPU-bound
// children (all nice 0, so all start in Q1). The parent then raises
// itself to Q2, so that when it wakes from pause() it preempts the
// children at the next tick (Rule 1) and changes their nice values
// while they are still partway through their Q1 allotment:
//   parent:  nice -10 -> "level P 1->2" right after the forks
//   child A: nice -15 -> starting queue Q2: "level A 1->2" at the
//            nice() call
//   child B: nice   3 -> starting queue still Q1: no level line, and
//            its used time is not reset, so "level B 1->0" comes once
//            its Q1 total reaches 10 ticks, not 10 ticks after nice()
//   child C: nice  12 -> starting queue Q0: "level C 1->0" at the
//            nice() call
// The "level P a->b at T" and "nice set to N for P" log lines show
// each move. If the children are still running 60 ticks after
// set_scheduler(3), the Rule 5 boost also adds "level P 0->..." lines.

#include "kernel/types.h"
#include "user/user.h"

#define NCHILD 3
#define SPIN   50 // ticks of elapsed time each child keeps the CPU busy
#define WAIT   15 // ticks the parent sleeps before changing nice values

// Use the CPU until SPIN ticks have elapsed. A child that spins across
// ticks is charged for them, unlike a pause(1) loop.
static void
spin(void)
{
  int end = uptime() + SPIN;
  volatile int x = 0;

  while (uptime() < end)
    x++;
}

int
main(void)
{
  int pid[NCHILD];
  int inc[NCHILD] = { -15, 3, 12 };

  if (set_scheduler(3) < 0) {
    printf("mlfqtest: set_scheduler(3) failed\n");
    exit(1);
  }
  startLogging();

  // Fork before the parent does any CPU work of its own.
  for (int i = 0; i < NCHILD; i++) {
    pid[i] = fork();
    if (pid[i] < 0) {
      printf("mlfqtest: fork failed\n");
      exit(1);
    }
    if (pid[i] == 0) {
      spin();
      exit(0);
    }
  }

  // Move the parent to Q2. pause() wakes it at every tick, and each
  // wakeup preempts the running Q1 child (Rule 1), so the children
  // take turns and each has used about WAIT/3 ticks of its allotment
  // when the parent wakes for good. The parent sleeps again before
  // the next tick, so it is never charged and stays in Q2.
  nice(getpid(), -10);
  pause(WAIT);
  for (int i = 0; i < NCHILD; i++)
    nice(pid[i], inc[i]);

  for (int i = 0; i < NCHILD; i++)
    wait(0);
  stopLogging();

  printf("mlfqtest: parent=%d children A=%d B=%d C=%d, nice changes %d %d %d\n",
         getpid(), pid[0], pid[1], pid[2], inc[0], inc[1], inc[2]);
  exit(0);
}
