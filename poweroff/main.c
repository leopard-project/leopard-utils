#include "xtuiapi.h"   /* xapi_* + libsys.h: XPOWER_SHUTDOWN / XPOWER_REBOOT */

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    uint64_t action = XPOWER_SHUTDOWN;

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        xapi_Printf("The system will reboot now...\n");
        action = XPOWER_REBOOT;
    } else {
        xapi_Printf("The system will poweroff now...\n");
    }

    int64_t rc = (int64_t)xapi_PowerAction(action);
    xapi_Printf("poweroff: action failed rc=%lld\n", (long long)rc);
    return 1;
}

