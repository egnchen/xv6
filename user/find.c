#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "user/user.h"

char *
dir_push(char *dst, const char *src, int sz)
{
    int l1 = strlen(dst), l2 = strlen(src);
    if(l1 + l2 + 2 > sz) {
        return 0;
    }
    char *p = dst + l1;
    *p++ = '/';
    memmove(p, src, l2 + 1);
    return dst;
}

const char *
dir_name(const char *dir)
{
    const char *p = dir + strlen(dir) - 1;
    while(p >= dir && *p != '/') p--;
    if(p < dir) return dir;
    return p + 1;
}

char *
dir_pop(char *dir)
{
    char *last = (char *)dir_name(dir);
    if(last == dir) *dir = '\0';
    else *(last - 1) = '\0';
    return dir;
}

void
find(char *dir, const char *name, int sz)
{
    int fd;
    struct stat st;
    struct dirent de;

    // printf("Finding %s\n", dir);

    if((fd = open(dir, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", dir);
        return;
    }

    if(fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", dir);
        goto exit;
    }

    switch(st.type) {
    case T_FILE:
    case T_DEVICE:
        if(strcmp(dir_name(dir), name) == 0) {
            printf("%s\n", dir);
        }
        break;
    case T_DIR:
        while(read(fd, &de, sizeof(de)) == sizeof(de)) {
            if(de.inum == 0) {
                continue;
            }
            if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) {
                continue;
            }
            dir = dir_push(dir, de.name, sz);
            if(!dir) {
                fprintf(2, "find: path too long\n");
                exit(-1);
            }
            find(dir, name, sz);
            dir_pop(dir);
        }
        break;
    }
exit:
    close(fd);
}

int
main(int argc, char *argv[])
{
    if(argc != 3) {
        printf("Usage: find <directory> <pattern>\n");
        exit(-1);
    }

    char buf[512] = "\0";
    if(strlen(argv[1]) > sizeof(buf) - 1) {
        printf("find: dir too long\n");
        exit(-1);
    }
    strcpy(buf, argv[1]);
    find(buf, argv[2], sizeof(buf));
    exit(0);
}