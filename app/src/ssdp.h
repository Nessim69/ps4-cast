// ssdp.h - minimal UPnP/DLNA discovery responder.
#ifndef PS4CAST_SSDP_H
#define PS4CAST_SSDP_H

int ssdp_start(const char *ip, int http_port);
const char *ssdp_status(void);
// Send ssdp:byebye for every NT this device advertises. Call once at app
// exit so control points that saw our ssdp:alive drop us immediately instead
// of waiting out CACHE-CONTROL's max-age.
void ssdp_shutdown(void);
// This install's per-console UUID ("uuid:xxxxxxxx-...", RFC 4122 v4-style),
// persisted to /data so it survives a restart. Shared with description.xml's
// UDN (httpd.c) so both agree on one identity per console instead of every
// install advertising the same hard-coded UUID. Safe to call before
// ssdp_start (lazily loads/creates on first call either way).
const char *ssdp_uuid(void);

#endif
