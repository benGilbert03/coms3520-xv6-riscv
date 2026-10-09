#include "kernel/types.h"
#include "user/user.h"

static int failures = 0;

static void
check(char *what, int got, int want)
{
  if (got == want) {
    printf("ok:   %s = %d\n", what, got);
  } else {
    printf("FAIL: %s = %d, expected %d\n", what, got, want);
    failures++;
  }
}

// Set pid's nice value to exactly target (within range) using increments.
static int
setnice(int pid, int target)
{
  return nice(pid, target - nice(pid, 0));
}

// Fork a child that blocks until release() closes the write end.
static int
blocked_child(int fds[2])
{
  char c;
  int pid;

  if (pipe(fds) < 0) {
    printf("nicetest: pipe failed\n");
    exit(1);
  }
  if ((pid = fork()) == 0) {
    close(fds[1]);
    read(fds[0], &c, 1);
    exit(0);
  }
  close(fds[0]);
  return pid;
}

static void
release(int fds[2])
{
  close(fds[1]);
  wait(0);
}

int
main(void)
{
  int pid = getpid();
  int child, status;
  int fds[2];

  check("N0 default nice(pid, 0)", nice(pid, 0), 0);

  // N1/N2: two steps from non-zero values, so "set to inc" can't pass,
  // and re-read with nice(pid, 0) to show the value was stored.
  check("N1 nice(pid, 5) from 0", nice(pid, 5), 5);
  check("N1 nice(pid, 3) from 5", nice(pid, 3), 8);
  check("N1 stored value", nice(pid, 0), 8);
  check("N2 nice(pid, -11) from 8", nice(pid, -11), -3);
  check("N2 stored value", nice(pid, 0), -3);

  // C1/C2: approach each limit from one inside, go one past, then use
  // the int extremes to check for overflow.
  check("C1 set to 18", setnice(pid, 18), 18);
  check("C1 18 + 1", nice(pid, 1), 19);
  check("C1 19 + 1 (past max)", nice(pid, 1), 19);
  check("C1 19 + INT_MAX", nice(pid, 2147483647), 19);
  check("C2 set to -19", setnice(pid, -19), -19);
  check("C2 -19 - 1", nice(pid, -1), -20);
  check("C2 -20 - 1 (past min)", nice(pid, -1), -20);
  check("C2 -20 + INT_MIN", nice(pid, -2147483647 - 1), -20);

  startLogging();

  // I1: caller at 7, so a bug that changes the caller would show 8.
  // No "nice set to ... for 99999/0/-1" line may appear.
  check("I1 set own nice to 7", setnice(pid, 7), 7);
  printf("I1 marker: no nice line for 99999, 0, or -1 until I1 end\n");
  check("I1 nice(99999, 1)", nice(99999, 1), 0);
  check("I1 nice(0, 1)", nice(0, 1), 0);
  check("I1 nice(-1, 1)", nice(-1, 1), 0);
  printf("I1 end marker\n");
  check("I1 own value unchanged", nice(pid, 0), 7);

  // I2: control. A valid pid can legitimately return 0; the log line
  // "nice set to 0 for <child>" is what distinguishes it from I1.
  child = blocked_child(fds);
  check("I2 set child to -3", setnice(child, -3), -3);
  printf("I2 marker: expect 'nice set to 0 for %d' next\n", child);
  check("I2 nice(child, 3)", nice(child, 3), 0);
  release(fds);

  // I3: an exited but not yet reaped (zombie) child is invalid.
  if ((child = fork()) == 0)
    exit(0);
  pause(40);
  printf("I3 marker: no nice line for zombie %d\n", child);
  check("I3 nice(zombie, 1)", nice(child, 1), 0);
  printf("I3 end marker\n");
  wait(0);

  stopLogging();

  // F1: parent at 19, so a fork that copies the parent's value shows 19.
  check("F1 set parent to 19", setnice(pid, 19), 19);
  if (fork() == 0)
    exit(nice(getpid(), 0));
  wait(&status);
  check("F1 child nice right after fork", status, 0);
  check("F1 parent unchanged", nice(pid, 0), 19);

  // F2: a reaped -4 child's slot is reused by the next fork (allocproc
  // takes the first UNUSED slot); the new child must still start at 0.
  child = blocked_child(fds);
  check("F2 set first child to -4", setnice(child, -4), -4);
  release(fds);
  if (fork() == 0)
    exit(nice(getpid(), 0));
  wait(&status);
  check("F2 next child (reused slot) nice", status, 0);

  if (failures == 0)
    printf("nicetest: ALL PASSED\n");
  else
    printf("nicetest: %d FAILED\n", failures);
  exit(failures);
}
