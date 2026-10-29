#include "xtuiapi.h"   /* xapi_* + libsys.h: XPOWER_SHUTDOWN / XPOWER_REBOOT */

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        xapi_Printf("The system will reboot now...\n");
        xapi_PowerAction(XPOWER_REBOOT);
    } else {
        xapi_Printf("The system will poweroff now...\n");
        xapi_PowerAction(XPOWER_SHUTDOWN);
    }

    xapi_Printf("poweroff: action failed\n");
    return 1;
}

