#include "log.h"

#include <stdio.h>

static FILE *log_file;

void log_open(const char *path) { log_file = fopen(path, "w"); }

void log_close(void)
{
    if (log_file) fclose(log_file);
    log_file = NULL;
}

void log_vmsg(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    vfprintf(stderr, fmt, ap);
    if (log_file) {
        vfprintf(log_file, fmt, copy);
        fflush(log_file);
    }
    va_end(copy);
}

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(fmt, ap);
    va_end(ap);
}
