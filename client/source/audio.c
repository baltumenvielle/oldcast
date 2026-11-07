#include "audio.h"

#include <string.h>

#include "adpcm.h"
#include "common.h"

#define CHANNEL 0
#define BUFFERS 24
#define MAX_FRAMES 960 // 20 ms at 48 kHz; packets are 10 ms
#define BUFFER_MS 10

#define TARGET_START 4 // buffers (40 ms)
#define TARGET_MIN 2
#define TARGET_MAX 15
#define DROP_MARGIN 4
#define TARGET_DECAY_MS 15000
// An empty queue only counts as an underrun if packets were still arriving: when nothing
// plays on the PC, WASAPI loopback sends nothing and the queue empties normally.
#define SOURCE_ACTIVE_MS 60

#define HEADER_SIZE 20
#define STATE_SIZE 4

AudioStats g_audio_stats;

static bool s_available;
static ndspWaveBuf s_bufs[BUFFERS];
static s16 *s_pcm;
static bool s_paused;
static u32 s_target;
static u32 s_rate;
static int s_next_seq = -1;
static u64 s_last_packet;
static u64 s_last_underrun;
static LightLock s_lock;

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 queued_buffers(void)
{
    u32 n = 0;
    for (int i = 0; i < BUFFERS; i++)
        if (s_bufs[i].status == NDSP_WBUF_QUEUED || s_bufs[i].status == NDSP_WBUF_PLAYING)
            n++;
    return n;
}

static ndspWaveBuf *free_buffer(void)
{
    for (int i = 0; i < BUFFERS; i++)
        if (s_bufs[i].status == NDSP_WBUF_FREE || s_bufs[i].status == NDSP_WBUF_DONE)
            return &s_bufs[i];
    return NULL;
}

static void set_paused(bool paused)
{
    if (paused != s_paused) {
        ndspChnSetPaused(CHANNEL, paused);
        s_paused = paused;
    }
}

bool audio_init(void)
{
    LightLock_Init(&s_lock);
    if (R_FAILED(ndspInit()))
        return false;

    s_pcm = linearAlloc(BUFFERS * MAX_FRAMES * 2 * sizeof(s16));
    if (!s_pcm) {
        ndspExit();
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    s_available = true;
    audio_reset();
    return true;
}

void audio_exit(void)
{
    if (!s_available)
        return;
    ndspChnWaveBufClear(CHANNEL);
    ndspExit();
    linearFree(s_pcm);
    s_pcm = NULL;
    s_available = false;
}

bool audio_available(void)
{
    return s_available;
}

void audio_reset(void)
{
    if (!s_available)
        return;
    LightLock_Lock(&s_lock);
    ndspChnWaveBufClear(CHANNEL);
    ndspChnReset(CHANNEL);
    // The DSP mixes at about 32.7 kHz; polyphase gives the cleanest resampling from 48 kHz.
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_POLYPHASE);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(CHANNEL, mix);
    s_rate = 0;

    memset(s_bufs, 0, sizeof(s_bufs));
    for (int i = 0; i < BUFFERS; i++) {
        s_bufs[i].data_pcm16 = s_pcm + i * MAX_FRAMES * 2;
        s_bufs[i].status = NDSP_WBUF_FREE;
    }
    s_paused = false;
    set_paused(true);
    s_target = TARGET_START;
    s_next_seq = -1;
    s_last_packet = 0;
    s_last_underrun = svcGetSystemTick();
    memset(&g_audio_stats, 0, sizeof(g_audio_stats));
    g_audio_stats.target_ms = s_target * BUFFER_MS;
    LightLock_Unlock(&s_lock);
}

void audio_handle_packet(const u8 *data, size_t size)
{
    if (!s_available || size < HEADER_SIZE + 2 * STATE_SIZE || memcmp(data, "3DSA", 4) != 0 || data[4] != 1 ||
        data[5] != 2)
        return;
    u16 seq = rd16(data + 6);
    u32 rate = rd32(data + 8);
    u32 frames = rd16(data + 12);
    if (frames == 0 || frames > MAX_FRAMES || (frames & 1) || size < HEADER_SIZE + 2 * STATE_SIZE + frames)
        return;

    LightLock_Lock(&s_lock);
    g_audio_stats.packets++;
    if (s_next_seq >= 0)
        g_audio_stats.lost += (u16)(seq - s_next_seq);
    s_next_seq = (u16)(seq + 1);
    s_last_packet = svcGetSystemTick();

    if (rate != s_rate) {
        ndspChnSetRate(CHANNEL, (float)rate);
        s_rate = rate;
    }

    u32 queued = queued_buffers();
    if (queued >= s_target + DROP_MARGIN) {
        g_audio_stats.dropped++;
        LightLock_Unlock(&s_lock);
        return;
    }
    ndspWaveBuf *buf = free_buffer();
    if (!buf) {
        g_audio_stats.no_buffer++;
        LightLock_Unlock(&s_lock);
        return;
    }

    const u8 *codes = data + HEADER_SIZE + 2 * STATE_SIZE;
    for (int ch = 0; ch < 2; ch++) {
        const u8 *state = data + HEADER_SIZE + ch * STATE_SIZE;
        adpcm_decode(codes + ch * frames / 2, (int)frames, (s16)rd16(state), state[2], buf->data_pcm16 + ch, 2);
    }
    buf->nsamples = frames;
    buf->looping = false;
    DSP_FlushDataCache(buf->data_pcm16, frames * 2 * sizeof(s16));
    ndspChnWaveBufAdd(CHANNEL, buf);

    if (s_paused && queued + 1 >= s_target)
        set_paused(false);
    LightLock_Unlock(&s_lock);
}

void audio_tick(void)
{
    if (!s_available)
        return;
    LightLock_Lock(&s_lock);
    u64 now = svcGetSystemTick();
    u32 queued = queued_buffers();
    g_audio_stats.queued_ms = queued * BUFFER_MS;

    if (!s_paused && queued == 0) {
        set_paused(true);
        if (s_last_packet && TICKS_TO_MS(now - s_last_packet) < SOURCE_ACTIVE_MS) {
            g_audio_stats.underruns++;
            if (s_target < TARGET_MAX)
                s_target++;
            s_last_underrun = now;
        }
    } else if (s_target > TARGET_MIN && TICKS_TO_MS(now - s_last_underrun) > TARGET_DECAY_MS) {
        s_target--;
        s_last_underrun = now;
    }
    g_audio_stats.target_ms = s_target * BUFFER_MS;
    LightLock_Unlock(&s_lock);
}
