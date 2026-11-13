#include <stdio.h>
#include "xtuiapi.h"
#include "libsys.h"

int main(int argc, char *argv[], char *envp[])
{
    (void)envp;
    if (argc < 2) { printf("usage: umount mountpoint\n"); return 2; }
    int64_t rc = (int64_t)enter_syscall(SYS_UMOUNT2, (uint64_t)argv[1], 0, 0, 0, 0, 0);
    if (rc != 0) { printf("umount: %s failed (rc=%lld)\n", argv[1], (long long)rc); return 1; }
    return 0;
}
