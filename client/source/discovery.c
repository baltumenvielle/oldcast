#include "discovery.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

#define QUERY_INTERVAL_MS 1000
#define HOST_TIMEOUT_MS 3500
#define CLIENT_ID_PATH DATA_DIR "/client_id"

typedef struct __attribute__((packed)) {
    char magic[4]; // "OCDQ"
    u8 version;
    u8 reserved[3];
    u8 client_id[8];
} DiscoveryQuery;

typedef struct __attribute__((packed)) {
    char magic[4]; // "OCDA"
    u8 version;
    u8 flags;
    u8 reserved[2];
    u8 host_id[8];
    char name[32];
} DiscoveryAnswer;

_Static_assert(sizeof(DiscoveryQuery) == 16, "OCDQ is 16 bytes");
_Static_assert(sizeof(DiscoveryAnswer) == 48, "OCDA is 48 bytes");

static int s_sock = -1;
static u64 s_next_query;
static u32 s_subnet_broadcast;
static HostEntry s_hosts[MAX_HOSTS];
static int s_count;
static u8 s_client_id[8];
static bool s_have_id;

const u8 *client_id(void)
{
    if (s_have_id)
        return s_client_id;

    FILE *f = fopen(CLIENT_ID_PATH, "rb");
    if (f) {
        s_have_id = fread(s_client_id, 1, sizeof(s_client_id), f) == sizeof(s_client_id);
        fclose(f);
    }
    if (!s_have_id) {
        // Not secret, just stable: identifies this console to the PCs it pairs with.
        u64 seed = svcGetSystemTick() ^ ((u64)gethostid() << 32);
        for (int i = 0; i < 8; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            s_client_id[i] = (u8)(seed >> 56);
        }
        ensure_data_dir();
        f = fopen(CLIENT_ID_PATH, "wb");
        if (f) {
            fwrite(s_client_id, 1, sizeof(s_client_id), f);
            fclose(f);
        }
        s_have_id = true;
    }
    return s_client_id;
}

bool discovery_start(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0)
        return false;
    fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) | O_NONBLOCK);

    struct in_addr ip, netmask, broadcast;
    s_subnet_broadcast = 0;
    if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) == 0)
        s_subnet_broadcast = broadcast.s_addr;

    s_count = 0;
    s_next_query = 0;
    client_id();
    return true;
}

void discovery_stop(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static void send_query(u32 addr)
{
    DiscoveryQuery q;
    memset(&q, 0, sizeof(q));
    memcpy(q.magic, "OCDQ", 4);
    q.version = 1;
    memcpy(q.client_id, client_id(), sizeof(q.client_id));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(CONTROL_PORT);
    to.sin_addr.s_addr = addr;
    sendto(s_sock, &q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to));
}

static void remember(u32 addr, const DiscoveryAnswer *a, u64 now)
{
    HostEntry *entry = NULL;
    for (int i = 0; i < s_count; i++)
        if (memcmp(s_hosts[i].host_id, a->host_id, 8) == 0)
            entry = &s_hosts[i];
    if (!entry) {
        if (s_count == MAX_HOSTS)
            return;
        entry = &s_hosts[s_count++];
    }
    entry->addr = addr;
    memcpy(entry->host_id, a->host_id, 8);
    memcpy(entry->name, a->name, 32);
    entry->name[32] = 0;
    entry->flags = a->flags;
    entry->last_seen = now;
}

void discovery_poll(void)
{
    if (s_sock < 0)
        return;
    u64 now = svcGetSystemTick();

    if (now >= s_next_query) {
        send_query(htonl(INADDR_BROADCAST));
        if (s_subnet_broadcast && s_subnet_broadcast != htonl(INADDR_BROADCAST))
            send_query(s_subnet_broadcast);
        s_next_query = now + (u64)(QUERY_INTERVAL_MS * CPU_TICKS_PER_MSEC);
    }

    DiscoveryAnswer a;
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    while (recvfrom(s_sock, &a, sizeof(a), 0, (struct sockaddr *)&from, &from_len) == (ssize_t)sizeof(a)) {
        if (memcmp(a.magic, "OCDA", 4) == 0 && a.version == 1)
            remember(from.sin_addr.s_addr, &a, now);
        from_len = sizeof(from);
    }

    // Forget hosts that stopped answering.
    for (int i = 0; i < s_count;) {
        if (TICKS_TO_MS(now - s_hosts[i].last_seen) > HOST_TIMEOUT_MS)
            s_hosts[i] = s_hosts[--s_count];
        else
            i++;
    }
}

int discovery_hosts(const HostEntry **hosts)
{
    *hosts = s_hosts;
    return s_count;
}
