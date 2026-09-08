#include <stdio.h>

#include "log.h"

void platformLog(const logType type, const char *format, va_list va)
{
    FILE *out = stderr;

    switch (type) {
        case LOG_TYPE_NORMAL:
            out = stdout;
            break;
        case LOG_TYPE_WARNING:
            fputs("Warning: ", out);
            break;
        case LOG_TYPE_ERROR:
            fputs("Error: ", out);
            break;
        case LOG_TYPE_DEBUG:
            fputs("Debug: ", out);
            break;
    }

    vfprintf(out, format, va);
}
