/* Logs to stderr and to fpeb.log next to the program, so players can send us the file. */
#pragma once
#include <stdarg.h>

void log_open(const char *path);
void log_close(void);
void log_msg(const char *fmt, ...);
void log_vmsg(const char *fmt, va_list ap);
