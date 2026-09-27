// watchdog.h — freeze watchdog hooks (implemented in main.c).
//
// Only the main thread and the opener worker feed the watchdog; calls from
// other threads are ignored, so worker code (aseg, hls, httpsrc, tls) may call
// these freely around bounded slow I/O.
#ifndef PS4CAST_WATCHDOG_H
#define PS4CAST_WATCHDOG_H

// Pet the main heartbeat (or the opener's progress beat).
void watchdog_kick(void);
// Name the blocking call for a HANG line; returns the previous note so the
// caller can restore it.
const char *watchdog_note(const char *w);
// Opener worker entering/leaving a request (OPENHANG beat).
void watchdog_open_job(int on);
// Main thread about to block on a playback switch (1): longer grace until
// the main loop clears it (0).
void watchdog_set_busy(int on);

#endif
