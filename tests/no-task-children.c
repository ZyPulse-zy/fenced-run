// SPDX-License-Identifier: GPL-2.0-only
// Link-time test wrapper: simulate kernels without CONFIG_CHECKPOINT_RESTORE.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    if (strstr(path, "/proc/self/task/") && strstr(path, "/children")) {
        errno = ENOENT; return -1;
    }
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags);
        mode_t mode = va_arg(args, mode_t); va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}
