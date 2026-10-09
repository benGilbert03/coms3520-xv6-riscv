#include "kernel/types.h"
#include "user/user.h"

// Iterations of CPU-bound work each child does in priority mode;
// enough to span several timer ticks.
#define WORK 100000000

// Child side of priority mode: tell the parent we're ready, block until
// the parent opens the gate, then report, do bounded CPU-bound work,
// report again, and exit.
static void
child(int ready[2], int gate[2])
{
  char c = 'r';
  int pid = getpid();
  int start, end;
  volatile uint64 x = 0;

  close(ready[0]);
  close(gate[1]);

  // Once the parent has seen both ready bytes, each child is either
  // blocked in the read below or RUNNABLE just before it, so after the
  // parent writes the gate bytes both children are RUNNABLE together,
  // no matter which one was forked or ran first.
  write(ready[1], &c, 1);
  close(ready[1]);
  read(gate[0], &c, 1);
  close(gate[0]);

  printf("child %d: nice %d, starting work\n", pid, nice(pid, 0));
  start = uptime();
  for (uint64 i = 0; i < WORK; i++)
    x += i;
  end = uptime();
  printf("child %d: work finished (ticks %d..%d)\n", pid, start, end);
  exit(0);
}

static void
priority(void)
{
  int ready[2], gate[2];
  int pid10, pid5, first, second;
  char buf[2];

  startLogging();

  // Highest priority, so the parent keeps the CPU while it sets up.
  nice(getpid(), -20);

  if (pipe(ready) < 0 || pipe(gate) < 0) {
    printf("logtest: pipe failed\n");
    exit(1);
  }

  if ((pid10 = fork()) == 0)
    child(ready, gate);
  if ((pid5 = fork()) == 0)
    child(ready, gate);
  if (pid10 < 0 || pid5 < 0) {
    printf("logtest: fork failed\n");
    exit(1);
  }

  // Children start at 0; assign their values before they can work.
  nice(pid10, 10);
  nice(pid5, 5);

  printf("logtest: prediction: child %d (nice 5) works before child %d (nice 10)\n",
         pid5, pid10);

  // Wait (sleeping, so the children can run) until both are blocked at the gate.
  close(ready[1]);
  close(gate[0]);
  for (int n = 0; n < 2;) {
    int r = read(ready[0], buf, 2 - n);
    if (r <= 0) {
      printf("logtest: ready handshake failed\n");
      exit(1);
    }
    n += r;
  }
  close(ready[0]);

  // Open the gate: one write wakes both children together. They compete
  // once the parent sleeps in wait().
  buf[0] = buf[1] = 'g';
  write(gate[1], buf, 2);
  close(gate[1]);

  first = wait(0);
  second = wait(0);

  printf("logtest: observed: child %d finished first, then child %d\n", first, second);
  if (first == pid5 && second == pid10)
    printf("logtest: priority PASS: nice 5 child had priority over nice 10 child\n");
  else
    printf("logtest: priority FAIL: expected %d then %d\n", pid5, pid10);

  stopLogging();
  exit(0);
}

int
main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "priority") == 0)
    priority();

  printf("logtest: calling startLogging\n");
  startLogging();
  printf("logtest: calling stopLogging\n");
  stopLogging();
  printf("logtest: done\n");
  exit(0);
}
