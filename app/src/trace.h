#ifndef PS4CAST_TRACE_H
#define PS4CAST_TRACE_H

void trace_mark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // -Wformat checks callers
const char *trace_path(void);

#endif
