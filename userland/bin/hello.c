/* rozOS — /bin/hello: আর্গুমেন্ট ও পরিবেশ দেখায়, তারপর exit 7 (init যাচাই করে) */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp)
{
    int i;
    printf("hello: pid %d, parent %d, argc %d\n", getpid(), getppid(), argc);
    for (i = 0; i < argc; i++) {
        printf("hello:   argv[%d] = \"%s\"\n", i, argv[i]);
    }
    for (i = 0; envp[i]; i++) {
        printf("hello:   env: %s\n", envp[i]);
    }
    return 7;
}
