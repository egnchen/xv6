#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int p[2];
    char buf = '\0';

    pipe(p);
    if(fork() == 0) {
        // child
        int pid = getpid();
        read(p[0], &buf, 1);
        printf("%d: received ping\n", pid);
        write(p[1], &buf, 1);
        close(p[0]);
        close(p[1]);
    } else {
        // parent
        int pid = getpid();
        write(p[1], &buf, 1);
        read(p[0], &buf, 1);
        printf("%d: received pong\n", pid);
        close(p[0]);
        close(p[1]);
    }
    exit(0);
}