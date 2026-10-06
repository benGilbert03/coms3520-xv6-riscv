#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main (int argc, char *argv[]) {
    printf("COM S 3520 Project 1A\n");
    printf("Student: Benjamin Gilbert\n");

    if (argc > 1) 
        printf("Argument: %s\n", argv[1]);
    exit(0);
}