#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main (int argc, char *argv[]) {
    if (argc > 1) 
        printf("Hello, %s, from xv6!\n", argv[1]);
    else 
        printf("Hello from xv6!\n");
    exit(0);
}