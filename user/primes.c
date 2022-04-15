#include "kernel/types.h"
#include "user/user.h"

void
run_child(int p[])
{
    int prime;
    int i;
    int np[2] = {-1, 0};

    close(p[1]);
    if(read(p[0], &prime, sizeof(prime))) {
        printf("prime %d\n", prime);
    } else {
        goto exit;
    }

    while(read(p[0], &i, sizeof(i))) {
        if(i % prime != 0) {
            if(np[0] == -1) {
                pipe(np);
                if(fork() == 0) {
                    // child
                    run_child(np);
                    // exits
                } else {
                    // parent
                    close(np[0]);
                }
            }
            write(np[1], &i, sizeof(i));
        }
    }
exit:
    close(p[0]);
    if(np[0] >= 0) {
        close(np[1]);
    }
    wait(0);
    exit(0);
}

int
main(int argc, char *argv[])
{
    // pipe to the second program in pipeline
    int p[2];
    pipe(p);

    if(fork() == 0) {
        // child
        run_child(p);
    } else {
        // parent
        close(p[0]);
        // feed 2 to 35 into pipe
        for(int i = 2; i <= 35; i++) {
            write(p[1], &i, sizeof(i));
        }
        close(p[1]);
        // wait for next one to exit
        wait(0);
    }
    exit(0);
}