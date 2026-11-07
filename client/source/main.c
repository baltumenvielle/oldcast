/**
 * @file main.c
 * @brief OldCast: low-latency game streaming client for the Old 3DS.
 *
 * The PC captures its screen, encodes it as MPEG-2 (which the Old 3DS can decode in
 * software at 400x240, 30 fps, unlike H.264) and sends it as MPEG-TS over UDP. This
 * client decodes and shows it on the top screen and sends the buttons back to the PC.
 *
 * Usage:
 *   - The home screen lists the PCs running the OldCast host (pc/oldcast): A connects,
 *     X only listens (for the measurement scripts in pc/), Y runs the offline benchmark.
 *   - With 3dslink: `3dslink -a <3ds-ip> -s oldcast.3dsx stream [seconds] [ab]` or
 *     `... bench` starts that mode directly, sends metrics to the PC and exits at the end.
 *     "ab" alternates two display configurations every 10 seconds (see stream.c).
 *     `... discover [seconds]` lists the PCs that answer discovery (protocol test), and
 *     `... connect [seconds]` connects to the first PC found.
 */
#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "bench.h"
#include "common.h"
#include "decoder.h"
#include "discovery.h"
#include "frame_queue.h"
#include "hosts.h"
#include "network.h"
#include "stream.h"
#include "video_out.h"

#define SOC_BUFFER_SIZE 0x100000

/** This console's user name from the system settings, as plain ASCII. */
static void console_name(char *out, size_t size)
{
    snprintf(out, size, "3DS");
    u16 utf16[14] = {0};
    if (R_FAILED(cfguInit()))
        return;
    if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(sizeof(utf16), 0x000A0000, utf16)) && utf16[0]) {
        size_t n = 0;
        for (size_t i = 0; i < 14 && utf16[i] && n + 1 < size; i++)
            out[n++] = utf16[i] < 0x80 ? (char)utf16[i] : '?';
        out[n] = 0;
    }
    cfguExit();
}

/**
 * The home list: PCs found on this network, then saved PCs that are not here but can be
 * reached at the public address they reported (marked HOST_REMOTE).
 */
static int build_list(HostEntry *out, int max)
{
    const HostEntry *found;
    int count = discovery_hosts(&found);
    if (count > max)
        count = max;
    memcpy(out, found, count * sizeof(HostEntry));

    for (int i = 0; i < hosts_count() && count < max; i++) {
        const SavedHost *saved = hosts_get(i);
        bool here = false;
        for (int j = 0; j < count; j++)
            if (memcmp(out[j].host_id, saved->host_id, 8) == 0)
                here = true;
        if (here || !saved->public_addr || !saved->has_key)
            continue;
        HostEntry *e = &out[count++];
        memset(e, 0, sizeof(*e));
        e->addr = saved->public_addr;
        e->port = saved->public_port;
        memcpy(e->host_id, saved->host_id, 8);
        memcpy(e->name, saved->name, sizeof(e->name));
        e->flags = HOST_REMOTE;
    }
    return count;
}

/** Connects to `host` and streams until the user stops. Returns false to quit the app. */
static bool connect_to(const HostEntry *host, u32 seconds)
{
    SessionRequest request;
    memset(&request, 0, sizeof(request));
    request.host_addr = host->addr;
    request.host_port = host->port;
    memcpy(request.host_id, host->host_id, 8);
    const SavedHost *saved = hosts_find(host->host_id);
    if (saved && saved->has_key) {
        request.has_key = true;
        memcpy(request.key, saved->key, sizeof(request.key));
    }
    request.pin = (u16)(1000 + svcGetSystemTick() % 9000);
    request.audio = audio_available();
    console_name(request.name, sizeof(request.name));

    char host_name[sizeof(host->name)];
    memcpy(host_name, host->name, sizeof(host_name));
    StreamOptions options = {.seconds = seconds, .session = &request, .host_name = host_name};
    return run_stream(&options);
}

static void draw_home_list(const HostEntry *hosts, int count, int cursor)
{
    printf("\x1b[1;1H" APP_NAME "\x1b[K\n\x1b[K\n");
    if (count == 0)
        printf("Looking for PCs on your network...\x1b[K\nOpen OldCast on your PC.\x1b[K\n\x1b[K\n");
    else
        printf("Choose your PC:\x1b[K\n\x1b[K\n\x1b[K\n");
    for (int i = 0; i < MAX_HOSTS; i++) {
        if (i < count) {
            struct in_addr a = {.s_addr = hosts[i].addr};
            const char *tag = (hosts[i].flags & HOST_REMOTE) ? "remote"
                              : (hosts[i].flags & 2)         ? "busy"
                              : (hosts[i].flags & 1)         ? "paired"
                                                             : "new";
            printf("%s %-18.18s %-6s\x1b[K\n", i == cursor ? ">" : " ", hosts[i].name, tag);
            printf("    %s\x1b[K\n", inet_ntoa(a));
        } else {
            printf("\x1b[K\n\x1b[K\n");
        }
    }
    printf("\x1b[K\nA connect  X listen  Y benchmark\x1b[K\nSTART quit\x1b[K\n");
}

/** Home screen: the list of PCs. Returns false when the app should close. */
static bool run_home(void)
{
    if (!discovery_start()) {
        printf("Cannot open the network.\nSTART: quit\n");
        return wait_for_key();
    }
    consoleClear();
    vo_show_status("Looking for PCs...", NULL, "Open OldCast on your PC");

    int cursor = 0, last_count = -1;
    u64 next_draw = 0;
    bool keep_app = true;
    while (aptMainLoop()) {
        discovery_poll();
        HostEntry hosts[MAX_HOSTS];
        int count = build_list(hosts, MAX_HOSTS);
        if (cursor >= count)
            cursor = count ? count - 1 : 0;

        gspWaitForVBlank();
        hidScanInput();
        u32 down = hidKeysDown();
        bool moved = false;
        if ((down & KEY_DOWN) && cursor + 1 < count) {
            cursor++;
            moved = true;
        }
        if ((down & KEY_UP) && cursor > 0) {
            cursor--;
            moved = true;
        }

        if (count != last_count) {
            vo_show_status(count ? "Choose your PC" : "Looking for PCs...", NULL,
                           count ? "A: connect" : "Open OldCast on your PC");
            last_count = count;
        }
        u64 now = svcGetSystemTick();
        if (moved || now >= next_draw) {
            draw_home_list(hosts, count, cursor);
            gfxFlushBuffers();
            next_draw = now + (u64)(500 * CPU_TICKS_PER_MSEC);
        }

        if (down & KEY_START) {
            keep_app = false;
            break;
        }
        if ((down & KEY_A) && count) {
            HostEntry chosen = hosts[cursor];
            discovery_stop();
            keep_app = connect_to(&chosen, 0);
            if (!keep_app || !discovery_start())
                return false;
            consoleClear();
            last_count = -1;
        } else if (down & KEY_X) {
            discovery_stop();
            StreamOptions options = {0};
            vo_show_status("Listening for video", NULL, "Hold SELECT+START to stop");
            keep_app = run_stream(&options);
            if (!keep_app || !discovery_start())
                return false;
            consoleClear();
            last_count = -1;
        } else if (down & KEY_Y) {
            discovery_stop();
            keep_app = run_bench();
            if (!keep_app || !discovery_start())
                return false;
            consoleClear();
            last_count = -1;
        }
    }
    discovery_stop();
    return keep_app;
}

/** Protocol test: looks for PCs and reports what answers, on screen and to the PC. */
static void run_discover(u32 seconds)
{
    consoleClear();
    if (!discovery_start()) {
        printf("Cannot open the discovery socket.\n");
        return;
    }
    const u8 *id = client_id();
    printf("Looking for PCs (%lu s)\nthis 3DS: %02x%02x%02x%02x%02x%02x%02x%02x\n\n", seconds, id[0], id[1], id[2],
           id[3], id[4], id[5], id[6], id[7]);
    if (g_linked)
        fprintf(stderr, "# discover: client id %02x%02x%02x%02x%02x%02x%02x%02x\n", id[0], id[1], id[2], id[3], id[4],
                id[5], id[6], id[7]);

    u64 start = svcGetSystemTick();
    u64 next_report = start;
    while (aptMainLoop() && TICKS_TO_MS(svcGetSystemTick() - start) < seconds * 1000.0) {
        discovery_poll();
        if (svcGetSystemTick() >= next_report) {
            const HostEntry *hosts;
            int count = discovery_hosts(&hosts);
            double t = TICKS_TO_MS(svcGetSystemTick() - start) / 1000.0;
            printf("\x1b[5;1H%.0f s: %d PC(s)\x1b[K\n", t, count);
            for (int i = 0; i < count; i++) {
                struct in_addr a = {.s_addr = hosts[i].addr};
                printf("  %-20s %s\x1b[K\n", hosts[i].name, inet_ntoa(a));
                if (g_linked)
                    fprintf(stderr, "%.1f,%s,%s,%u\n", t, hosts[i].name, inet_ntoa(a), hosts[i].flags);
            }
            if (g_linked && count == 0)
                fprintf(stderr, "%.1f,none\n", t);
            next_report += (u64)(1000 * CPU_TICKS_PER_MSEC);
        }
        svcSleepThread(20 * 1000000LL);
    }
    discovery_stop();
}

/** Runs the mode requested on the command line. Returns false if it was unknown. */
static bool run_auto_mode(int argc, char **argv)
{
    const char *mode = argv[1];
    if (strcmp(mode, "stream") == 0) {
        StreamOptions options = {
            .seconds = argc > 2 ? (u32)strtoul(argv[2], NULL, 10) : 0,
            .ab_test = argc > 3 && strcmp(argv[3], "ab") == 0,
        };
        run_stream(&options);
        return true;
    }
    if (strcmp(mode, "bench") == 0) {
        run_bench();
        return true;
    }
    if (strcmp(mode, "discover") == 0) {
        run_discover(argc > 2 ? (u32)strtoul(argv[2], NULL, 10) : 10);
        return true;
    }
    if (strcmp(mode, "connect") == 0) {
        // Find the first PC (up to 5 s), then stream from it.
        if (!discovery_start())
            return true;
        u64 start = svcGetSystemTick();
        const HostEntry *hosts;
        while (aptMainLoop() && discovery_hosts(&hosts) == 0 && TICKS_TO_MS(svcGetSystemTick() - start) < 5000) {
            discovery_poll();
            svcSleepThread(20 * 1000000LL);
        }
        HostEntry chosen;
        bool found = discovery_hosts(&hosts) > 0;
        if (found)
            chosen = hosts[0];
        discovery_stop();
        if (found) {
            if (g_linked)
                fprintf(stderr, "# connecting to %s\n", chosen.name);
            connect_to(&chosen, argc > 2 ? (u32)strtoul(argv[2], NULL, 10) : 0);
        } else if (g_linked) {
            fprintf(stderr, "# no PC found\n");
        }
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true); // only has an effect on the New 3DS

    // The network service stays up for the whole run: the stream and the 3dslink
    // connection both use it.
    void *soc_buffer = memalign(0x1000, SOC_BUFFER_SIZE);
    bool soc_ok = soc_buffer && R_SUCCEEDED(socInit(soc_buffer, SOC_BUFFER_SIZE));
    if (soc_ok && __3dslink_host.s_addr)
        g_linked = link3dsStdioForDebug() >= 0;
    if (g_linked)
        fprintf(stderr, "# " APP_NAME " connected from %s\n", inet_ntoa((struct in_addr){.s_addr = gethostid()}));

    bool ok = soc_ok && vo_init() && dec_init() && fq_init();
    if (!ok) {
        printf("Initialization failed (network, video, decoder\nor memory).\nSTART: quit\n");
        wait_for_key();
    }
    // Audio is optional. The DSP needs sdmc:/3ds/dspfirm.cdc, dumped with DSP1.
    if (ok && !audio_init()) {
        printf("No audio: sdmc:/3ds/dspfirm.cdc is missing.\nRun DSP1 once to create it.\n\nA: continue\n");
        if (!wait_for_key())
            ok = false;
    }

    bool running = ok;
    if (running && argc > 1 && run_auto_mode(argc, argv))
        running = false; // launched for one job: exit when it is done

    if (running)
        run_home();

    if (ok) {
        audio_exit();
        fq_exit();
        dec_exit();
        vo_exit();
    }
    if (g_linked)
        fflush(stderr);
    if (soc_ok)
        socExit();
    free(soc_buffer);
    gfxExit();
    return 0;
}
