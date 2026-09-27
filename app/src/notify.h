// notify.h — on-screen system toast (survives even if the app crashes right
// after), used both for user feedback and as a crash localizer.
#ifndef PS4CAST_NOTIFY_H
#define PS4CAST_NOTIFY_H

// printf-style: lets -Wformat check every caller's arguments.
#define NOTIFY_PRINTF(f, a) __attribute__((format(printf, f, a)))

// Always shown. Reserve for things the user must see: ready, hard failures.
void notify(const char *fmt, ...) NOTIFY_PRINTF(1, 2);

// Diagnostic toast: only shown when debug mode is enabled (Settings -> Debug
// notifications). Default OFF for a clean, final-product TV display.
void notify_dbg(const char *fmt, ...) NOTIFY_PRINTF(1, 2);

void notify_set_debug(int on);
int  notify_get_debug(void);

#endif
