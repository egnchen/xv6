#include "kernel/types.h"
#include "user/user.h"

int
readline(int fd, char *buf, int sz)
{
    char *p = buf;
    while(p < buf + sz && read(fd, p, 1) == 1) {
        if(*p == '\n') {
            *p = '\0';
            return p - buf;
        }
        p++;
    }
    if(p == buf + sz) {
        fprintf(2, "Error: line too long\n");
        return 0;
    }
    // eof
    *p = '\0';
    return p - buf;
}

int
main(int argc, char *argv[])
{
    char buf[512];
    
    // construct new arg list
    char *nargv[argc];
    memcpy(nargv, argv + 1, sizeof(char *) * (argc - 1));
    nargv[argc - 1] = buf;

    // TODO maybe separate the args by space?
    while(readline(0, buf, sizeof(buf))) {
        if(fork() == 0) {
            exec(nargv[0], nargv);
        }
    }
    wait(0);
    exit(0);
}