#include "kernel/types.h"
#include "user/user.h"

const char *usage_prompt = "Usage: sleep <seconds>\n";

int
main(int argc, char *argv[])
{
    int sleep_time;

    if(argc != 2) {
        write(1, usage_prompt, strlen(usage_prompt));
        exit(-1);
    }

    sleep_time = atoi(argv[1]);
    sleep(sleep_time);
    
    exit(0);
}