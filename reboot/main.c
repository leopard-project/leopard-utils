#include <stdio.h>
#include "xtuiapi.h"

int main(int argc, char *argv[], char *envp[])
{
    (void)argc; (void)argv; (void)envp;
    printf("The system will reboot now...\n");
    int64_t rc = (int64_t)xapi_PowerAction(XPOWER_REBOOT);
    printf("reboot: action failed rc=%lld\n", (long long)rc);
    return 1;
}
