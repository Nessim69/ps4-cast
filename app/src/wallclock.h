// wallclock.h — calendar time for the programme guide.
//
// The rest of the app only needs monotonic time; the guide needs "now" in
// Unix seconds and the viewer's time zone to print 20:00 rather than 19:00.
// The zone comes from the console's own setting when libkernel will convert
// it, else from the phone browser that opened the web UI (same room, same
// zone), else UTC.
#ifndef PS4CAST_WALLCLOCK_H
#define PS4CAST_WALLCLOCK_H

#include <stdint.h>

int64_t wallclock_utc(void);            // Unix seconds, 0 if the clock is unset
int     wallclock_offset(void);         // seconds east of UTC, for local display
// Offset reported by the web UI (minutes east of UTC); persisted by httpd.
void    wallclock_set_phone_offset(int minutes);
int     wallclock_phone_offset(void);   // minutes; 0 if never reported
// "20:05" for a Unix time, in local time.
void    wallclock_hhmm(int64_t t, char *out, int cap);

#endif
