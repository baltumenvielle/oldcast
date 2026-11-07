/**
 * @file main.c
 * @brief OldCast: low-latency game streaming client for the Old 3DS.
 *
 * The PC captures its screen, encodes it as MPEG-2 (which the Old 3DS can decode in
 * software at 400x240, 30 fps, unlike H.264) and sends it as MPEG-TS over UDP. This
 * client decodes and shows it on the top screen and sends the buttons back to the PC.
 *
 * Usage:
 *   - From the Homebrew Launcher: A starts streaming, Y runs the offline benchmark.
 *   - With 3dslink: `3dslink -a <3ds-ip> -s oldcast.3dsx stream [seconds] [ab]` or
 *     `... bench` starts that mode directly, sends metrics to the PC and exits at the end.
 *     "ab" alternates two display configurations every 10 seconds (see stream.c).
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
#include "frame_queue.h"
#include "network.h"
#include "stream.h"
#include "video_out.h"

#define SOC_BUFFER_SIZE 0x100000

static void print_home(void)
{
    consoleClear();
    printf(APP_NAME " - Old 3DS\n\n");
    printf("  A      receive video (UDP %d)\n", VIDEO_PORT);
    printf("  Y      offline benchmark (bench.ts)\n");
    printf("  START  quit\n");
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
        print_home();
    while (running && aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START)
            break;
        if (down & KEY_A) {
            StreamOptions options = {0};
            running = run_stream(&options);
            print_home();
        } else if (down & KEY_Y) {
            running = run_bench();
            print_home();
        }
    }

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
