#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  printf("logtest: calling startLogging\n");
  startLogging();
  printf("logtest: calling stopLogging\n");
  stopLogging();
  printf("logtest: done\n");
  exit(0);
}
