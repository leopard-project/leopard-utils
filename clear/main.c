#include <stdio.h>

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    /* ANSI clear screen + home; conhost's VT parser handles it. */
    printf("\033[2J\033[H");
    return 0;
}
