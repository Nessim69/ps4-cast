// agent_proto.h — wire protocol of the resident deploy agent (TCP 9192).
//
// The agent installs a PKG from a manifest URL. It used to accept that
// command from ANY host on the LAN with no credentials, so anyone on the
// network could make the console install a package while it was resident.
// Every command is now authenticated with HMAC-SHA256 under a 32-byte secret
// that the deploy script generates when it bootstraps the agent and patches
// into the payload binary (AGENT_SECRET_PLACEHOLDER), keeping a copy in the
// git-ignored .ps4cast-agent-secret.
//
//   agent  -> client : "PCAG1" | nonce[32]                     (fresh per connection)
//   client -> agent  : "PCAR"  | len:u32le | packet[len] | mac[32]
//        mac = HMAC-SHA256(secret, "ps4cast-agent-v1" | nonce | len:u32le | packet)
//   agent  -> client : one status line ("READY ...", "ERROR ...", "BYE"), then close
//
// packet (the DirectPackageInstaller metadata layout, little-endian):
//   cmd:u32 (1 = install, 0 = stop the agent)
//   install: url:str name:str content_id:str pkg_type:str size:u64 icon_len:u32(=0)
//   str = len:u32 | bytes (no terminator)
//
// Pure code: compiled into the payload and by tests/host/test_agent_crypto.c.
#ifndef PS4CAST_AGENT_PROTO_H
#define PS4CAST_AGENT_PROTO_H

#include <stdint.h>

#define AGENT_GREETING       "PCAG1"
#define AGENT_GREETING_LEN   5
#define AGENT_REQUEST_MAGIC  "PCAR"
#define AGENT_MAC_LABEL      "ps4cast-agent-v1"
#define AGENT_MAX_PACKET     8192
#define AGENT_SECRET_LEN     32
// 32 bytes, no terminator: the deploy script replaces exactly this run of
// bytes in the built binary. The agent refuses to run while it is unpatched.
#define AGENT_SECRET_PLACEHOLDER "PS4CAST_AGENT_SECRET_PLACEHOLDER"

#define AGENT_CMD_STOP    0u
#define AGENT_CMD_INSTALL 1u

typedef struct {
    uint32_t cmd;
    char     url[0x800];
    char     name[0x259];
    char     id[0x30];
    char     type[0x10];
    uint64_t size;
} AgentRequest;

// MAC over one request (see above).
void agent_mac(const uint8_t secret[AGENT_SECRET_LEN], const uint8_t nonce[32],
               const uint8_t *packet, uint32_t len, uint8_t out[32]);

// Parse a verified packet. 0 = ok; -1 = malformed/truncated/oversized field.
int agent_parse(const uint8_t *packet, uint32_t len, AgentRequest *out);

#endif
