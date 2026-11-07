/**
 * @file discovery.h
 * @brief Finds PCs running the OldCast host on the local network.
 *
 * Broadcasts an OCDQ query on port 5007 once per second and collects the OCDA answers
 * (see docs/protocol.md). The query goes both to 255.255.255.255 and to the subnet's
 * broadcast address, since some routers only forward one of them.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>

#define CONTROL_PORT 5007
#define MAX_HOSTS 8

/** HostEntry flag: a saved PC that is not on this network, reached at its public address. */
#define HOST_REMOTE 0x80

typedef struct {
    u32 addr;       /**< IPv4 address, network byte order */
    u16 port;       /**< control port, 5007 unless set otherwise from outside */
    u8 host_id[8];
    char name[33];
    u8 flags;       /**< OCDA flags (bit 0 paired, bit 1 busy) or HOST_REMOTE */
    u64 last_seen;  /**< svcGetSystemTick() of the last answer */
} HostEntry;

/** Opens the discovery socket. @return false if it could not be created. */
bool discovery_start(void);
void discovery_stop(void);

/** Sends a query when one is due and reads the answers. Never blocks; call often. */
void discovery_poll(void);

/** Hosts that answered in the last few seconds. @return how many. */
int discovery_hosts(const HostEntry **hosts);

/** The 8-byte id of this 3DS, created once and stored on the SD card. */
const u8 *client_id(void);
