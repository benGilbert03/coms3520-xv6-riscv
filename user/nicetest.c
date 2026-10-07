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

int
main(void)
{
  int pid = getpid();
  int child, status;

  // Default value, positive and negative increments.
  check("default nice(pid, 0)", nice(pid, 0), 0);
  check("nice(pid, 5)", nice(pid, 5), 5);
  check("nice(pid, -8)", nice(pid, -8), -3);

  // Clamp boundaries: exact limits, then past them.
  check("nice(pid, 22) to max", nice(pid, 22), 19);
  check("nice(pid, 1) past max", nice(pid, 1), 19);
  check("nice(pid, 2147483647) past max", nice(pid, 2147483647), 19);
  check("nice(pid, -39) to min", nice(pid, -39), -20);
  check("nice(pid, -1) past min", nice(pid, -1), -20);
  check("nice(pid, -2147483647-1) past min", nice(pid, -2147483647 - 1), -20);

  // Nonexistent / invalid pids return 0.
  check("nice(99999, 1) nonexistent", nice(99999, 1), 0);
  check("nice(0, 1)", nice(0, 1), 0);
  check("nice(-1, 1)", nice(-1, 1), 0);
  check("own value unchanged", nice(pid, 0), -20);

  // A forked child starts at 0, not the parent's value.
  nice(pid, 100); // parent now 19
  child = fork();
  if (child == 0)
    exit(nice(getpid(), 0) == 0 ? 0 : 1);
  wait(&status);
  check("forked child started at 0 (exit status)", status, 0);

  // Another live process can be changed by pid.
  child = fork();
  if (child == 0) {
    pause(20);
    exit(0);
  }
  check("nice(child, -4) on live child", nice(child, -4), -4);
  // An exited but not yet reaped (zombie) child is invalid.
  pause(40);
  check("nice(child, 1) on zombie", nice(child, 1), 0);
  wait(0);

  // Logging: one line only while logging is on.
  startLogging();
  printf("expect a 'nice set to -5 for %d' line next:\n", pid);
  nice(pid, -24);
  stopLogging();
  printf("expect no nice line next:\n");
  nice(pid, 1);

  if (failures == 0)
    printf("nicetest: ALL PASSED\n");
  else
    printf("nicetest: %d FAILED\n", failures);
  exit(failures);
}
