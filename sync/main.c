#include <stdio.h>
#include "libsys.h"

#ifndef SYS_SYNC
#define SYS_SYNC 162
#endif

int main(int argc, char *argv[], char *envp[])
{
    (void)argc; (void)argv; (void)envp;
    enter_syscall(SYS_SYNC, 0, 0, 0, 0, 0, 0);
    return 0;
}
