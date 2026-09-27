// netmon.h — keep PS4 Cast reachable as the console's network comes and goes.
//
// The address used to be read once at launch: a console whose Wi-Fi was not
// up yet never started its web server or DLNA discovery, and a new DHCP lease
// left the TV, QR code and SSDP LOCATION pointing at a dead address until the
// app was relaunched. netmon polls the address (netwatch.c decides what
// counts as a change), starts the web server and SSDP once there is a
// network, moves SSDP to a new address and tells the user where to find the
// receiver now.
#ifndef PS4CAST_NETMON_H
#define PS4CAST_NETMON_H

// First poll runs synchronously (so a normal boot is exactly as before), then
// a thread keeps polling. Call once from main() after httpd_init().
void netmon_start(int http_port);
// Current LAN address ("" while there is none), and a counter that changes
// whenever it does.
void     netmon_ip(char *out, int cap);
unsigned netmon_generation(void);

#endif
