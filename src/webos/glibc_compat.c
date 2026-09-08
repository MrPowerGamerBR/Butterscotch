#include <sys/types.h>
#include <sys/stat.h>

extern int __xstat(int ver, const char *path, struct stat *buf);
extern int __fxstat(int ver, int fd, struct stat *buf);

int stat(const char *path, struct stat *buf)
{
    return __xstat(0, path, buf);
}

int fstat(int fd, struct stat *buf)
{
    return __fxstat(0, fd, buf);
}
