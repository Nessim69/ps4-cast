#include "netutil.h"

#include <string.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>

int net_init(void) {
    // The Net/NetCtl PRXs are also linked (-lSceNet/-lSceNetCtl), so these
    // loads are belt and braces. The call returns uint32_t: the old "< 0"
    // tests could never fire, and a real failure surfaces from
    // sceNetInit/sceNetCtlInit below either way.
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);

    if (sceNetInit() < 0)
        return -3;
    if (sceNetCtlInit() < 0)
        return -4;
    return 0;
}

int net_get_ip(char *out, int outlen) {
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    int rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc < 0)
        return rc;
    strncpy(out, info.ip_address, outlen - 1);
    out[outlen - 1] = '\0';
    return 0;
}
