#include <stdio.h>
#include <string.h>
#include "xtuiapi.h"
#include "libsys.h"

int main(int argc, char *argv[], char *envp[])
{
    const char *type = "fatfs";
    int first = 1;
    (void)envp;

    if (argc > 2 && !strcmp(argv[1], "-t")) { type = argv[2]; first = 3; }
    if (argc - first < 2) { printf("usage: mount [-t fstype] device mountpoint\n"); return 2; }

    int64_t rc = (int64_t)enter_syscall(SYS_MOUNT, (uint64_t)argv[first],
                                        (uint64_t)argv[first + 1], (uint64_t)type, 0, 0, 0);
    if (rc != 0) { printf("mount: %s -> %s failed (rc=%lld)\n", argv[first], argv[first + 1], (long long)rc); return 1; }
    return 0;
}
