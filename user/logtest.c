#include "kernel/types.h"
#include "user/user.h"

// Iterations of bounded CPU-bound work each child does;
// enough to span a few timer ticks.
#define WORK 100000000

// What each competing child reports back to the parent.
struct result {
  int pid;
  int start; // uptime() when its work began
  int end;   // uptime() when its work finished
};

static void
work(uint64 n)
{
  volatile uint64 x = 0;

  for (uint64 i = 0; i < n; i++)
    x += i;
}

// Child side of a competition: tell the parent we're ready, block until
// the parent opens the gate, optionally sleep, then do bounded CPU-bound
// work, report the ticks it spanned through the results pipe, and exit.
static void
child(int ready[2], int gate[2], int results[2], uint64 n, int delay)
{
  char c = 'r';
  struct result r;

  close(ready[0]);
  close(gate[1]);
  close(results[0]);

  // Once the parent has seen both ready bytes, each child is either
  // blocked in the read below or RUNNABLE just before it, so after the
  // parent writes the gate bytes both children are RUNNABLE together,
  // no matter which one was forked or ran first.
  write(ready[1], &c, 1);
  close(ready[1]);
  read(gate[0], &c, 1);
  close(gate[0]);

  if (delay > 0)
    pause(delay);

  r.pid = getpid();
  printf("child %d: nice %d, starting work\n", r.pid, nice(r.pid, 0));
  r.start = uptime();
  work(n);
  r.end = uptime();
  printf("child %d: work finished (ticks %d..%d)\n", r.pid, r.start, r.end);
  write(results[1], &r, sizeof(r));
  close(results[1]);
  exit(0);
}

// Fork child A (first) and child B (second) with the given nice values,
// work amounts, and B's sleep before working. Release them together,
// wait for both, and return their results in a[0] (A) and a[1] (B).
// *first gets the pid that wait() returned first.
static void
compete(int niceA, int niceB, uint64 workA, uint64 workB, int delayB,
        struct result a[2], int *first)
{
  int ready[2], gate[2], results[2];
  int pids[2];
  char buf[2];
  struct result r;

  startLogging();

  // Highest priority, so the parent keeps the CPU while it sets up.
  nice(getpid(), -20);

  if (pipe(ready) < 0 || pipe(gate) < 0 || pipe(results) < 0) {
    printf("logtest: pipe failed\n");
    exit(1);
  }

  if ((pids[0] = fork()) == 0)
    child(ready, gate, results, workA, 0);
  if ((pids[1] = fork()) == 0)
    child(ready, gate, results, workB, delayB);
  if (pids[0] < 0 || pids[1] < 0) {
    printf("logtest: fork failed\n");
    exit(1);
  }

  // Children start at 0; assign their values before they can work.
  nice(pids[0], niceA);
  nice(pids[1], niceB);
  printf("logtest: child A = %d (nice %d, forked first), child B = %d (nice %d)\n",
         pids[0], niceA, pids[1], niceB);

  // Wait (sleeping, so the children can run) until both are at the gate.
  close(ready[1]);
  close(gate[0]);
  close(results[1]);
  for (int n = 0; n < 2;) {
    int got = read(ready[0], buf, 2 - n);
    if (got <= 0) {
      printf("logtest: ready handshake failed\n");
      exit(1);
    }
    n += got;
  }
  close(ready[0]);

  // Open the gate: both children are RUNNABLE once the parent sleeps.
  buf[0] = buf[1] = 'g';
  write(gate[1], buf, 2);
  close(gate[1]);

  *first = wait(0);
  wait(0);

  for (int i = 0; i < 2; i++) {
    if (read(results[0], &r, sizeof(r)) != sizeof(r)) {
      printf("logtest: missing child result\n");
      exit(1);
    }
    a[r.pid == pids[0] ? 0 : 1] = r;
  }
  close(results[0]);

  stopLogging();
}

static void
verdict(char *id, int ok)
{
  printf("logtest: %s %s\n", id, ok ? "PASS" : "FAIL");
  exit(ok ? 0 : 1);
}

// The lower-nice child must finish first, and its work must not
// overlap the higher-nice child's (strict priority, not round-robin).
static void
priority(char *id, int niceA, int niceB)
{
  struct result a[2];
  int first, lo, hi;

  compete(niceA, niceB, WORK, WORK, 0, a, &first);
  lo = niceA < niceB ? 0 : 1;
  hi = 1 - lo;
  printf("logtest: lower-nice %d ran ticks %d..%d, higher-nice %d ran ticks %d..%d, "
         "first finished %d\n",
         a[lo].pid, a[lo].start, a[lo].end, a[hi].pid, a[hi].start, a[hi].end, first);
  verdict(id, first == a[lo].pid && a[lo].end <= a[hi].start);
}

// A nice-10 child is already working when a nice-5 child wakes up;
// the nice-5 child must take over, so its work falls inside the other's.
static void
preempt(void)
{
  struct result a[2];
  int first;

  compete(10, 5, 3 * WORK, WORK, 2, a, &first);
  printf("logtest: nice 10 %d ran ticks %d..%d, nice 5 %d ran ticks %d..%d\n",
         a[0].pid, a[0].start, a[0].end, a[1].pid, a[1].start, a[1].end);
  verdict("P3 preempt",
          first == a[1].pid && a[0].start < a[1].start && a[1].end <= a[0].end);
}

// Two equal-nice children must take turns, so their work overlaps.
static void
equal(void)
{
  struct result a[2];
  int first;

  compete(5, 5, 3 * WORK, 3 * WORK, 0, a, &first);
  printf("logtest: nice 5 %d ran ticks %d..%d, nice 5 %d ran ticks %d..%d\n",
         a[0].pid, a[0].start, a[0].end, a[1].pid, a[1].start, a[1].end);
  verdict("E1 equal", a[0].start < a[1].end && a[1].start < a[0].end);
}

int
main(int argc, char *argv[])
{
  if (argc > 1) {
    if (strcmp(argv[1], "priority") == 0)
      priority("P1 priority", 10, 5);
    if (strcmp(argv[1], "priority-rev") == 0)
      priority("P2 priority-rev", 5, 10);
    if (strcmp(argv[1], "preempt") == 0)
      preempt();
    if (strcmp(argv[1], "equal") == 0)
      equal();
    if (strcmp(argv[1], "quiet") == 0) {
      // Same work as below with logging never turned on.
      if (fork() == 0) {
        nice(getpid(), 10);
        work(WORK);
        exit(0);
      }
      wait(0);
      exit(0);
    }
    printf("usage: logtest [priority|priority-rev|preempt|equal|quiet]\n");
    exit(1);
  }

  startLogging();
  if (fork() == 0) {
    volatile uint64 x = 0;

    nice(getpid(), 10);
    for (uint64 i = 0; i < WORK; i++)
      x += i;
    exit(0);
  }
  wait(0);
  stopLogging();
  exit(0);
}
