// jb.c — credential elevation, in its own translation unit: ps4-libjbc's
// defs.h redefines pid_t & co. incompatibly with the FreeBSD socket headers
// agent.c needs (DPI keeps them apart the same way: main.c vs info.c).
#include <stdint.h>
#include "jailbreak.h"

void agent_elevate(void) {
    struct jbc_cred cred;
    jbc_get_cred(&cred);
    jbc_jailbreak_cred(&cred);
    cred.jdir = 0;
    cred.sceProcType = 0x3800000000000010;
    cred.sonyCred = 0x40001c0000000000;
    cred.sceProcCap = 0x900000000000ff00;
    jbc_set_cred(&cred);
}
