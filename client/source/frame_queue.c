#include "frame_queue.h"

#include "ts_demux.h"

#include <stdlib.h>
#include <string.h>

// libavcodec requires this much zeroed padding after the data.
#define PADDING 64

FqStats g_fq_stats;

static FqSlot s_slots[FQ_SLOTS];
static u32 s_head;  // oldest frame
static u32 s_count; // complete frames, including the one being decoded
static bool s_busy; // the frame at s_head is in use by the decoder
static bool s_need_key;
static u32 s_writing;
static u64 s_last_commit;
static LightLock s_lock;
static LightEvent s_event;

bool fq_init(void)
{
    LightLock_Init(&s_lock);
    LightEvent_Init(&s_event, RESET_ONESHOT);
    for (int i = 0; i < FQ_SLOTS; i++) {
        s_slots[i].data = malloc(FQ_SLOT_CAP + PADDING);
        if (!s_slots[i].data)
            return false;
    }
    fq_reset();
    return true;
}

void fq_exit(void)
{
    for (int i = 0; i < FQ_SLOTS; i++) {
        free(s_slots[i].data);
        s_slots[i].data = NULL;
    }
}

void fq_reset(void)
{
    LightLock_Lock(&s_lock);
    s_head = 0;
    s_count = 0;
    s_busy = false;
    s_need_key = true; // decoding cannot start from a P-frame
    s_last_commit = 0;
    memset(&g_fq_stats, 0, sizeof(g_fq_stats));
    LightLock_Unlock(&s_lock);
}

u8 *fq_begin(size_t *cap)
{
    LightLock_Lock(&s_lock);
    if (s_count == FQ_SLOTS) {
        u32 keep = s_busy ? 1 : 0;
        g_fq_stats.dropped += s_count - keep;
        g_fq_stats.overflows++;
        s_count = keep;
        s_need_key = true;
    }
    s_writing = (s_head + s_count) % FQ_SLOTS;
    LightLock_Unlock(&s_lock);

    *cap = FQ_SLOT_CAP;
    return s_slots[s_writing].data;
}

void fq_commit(size_t len, bool corrupt)
{
    FqSlot *slot = &s_slots[s_writing];
    u64 now = svcGetSystemTick();

    if (s_last_commit) {
        u32 gap = (u32)((now - s_last_commit) / CPU_TICKS_PER_MSEC);
        if (gap > g_fq_stats.max_gap_ms)
            g_fq_stats.max_gap_ms = gap;
    }
    s_last_commit = now;
    g_fq_stats.received++;

    if (len == 0) { // did not fit in the slot: the next P-frame would be broken
        LightLock_Lock(&s_lock);
        s_need_key = true;
        g_fq_stats.dropped++;
        LightLock_Unlock(&s_lock);
        return;
    }
    if (corrupt)
        g_fq_stats.corrupt++;

    memset(slot->data + len, 0, PADDING);
    slot->len = len;
    slot->type = mpeg2_picture_type(slot->data, len);
    slot->corrupt = corrupt;
    slot->t_ready = now;

    LightLock_Lock(&s_lock);
    s_count++;
    LightLock_Unlock(&s_lock);
    LightEvent_Signal(&s_event);
}

// Moves s_head to the newest I-frame in the queue. Returns false if there is none.
static bool jump_to_newest_key(void)
{
    for (u32 back = s_count; back > 0; back--) {
        u32 offset = back - 1;
        if (s_slots[(s_head + offset) % FQ_SLOTS].type == PIC_I) {
            g_fq_stats.dropped += offset;
            if (offset)
                g_fq_stats.late_jumps++;
            s_head = (s_head + offset) % FQ_SLOTS;
            s_count -= offset;
            return true;
        }
    }
    return false;
}

FqSlot *fq_acquire(s64 timeout_ns)
{
    LightLock_Lock(&s_lock);
    if (s_need_key) {
        if (jump_to_newest_key()) {
            s_need_key = false;
        } else {
            g_fq_stats.dropped += s_count;
            s_head = (s_head + s_count) % FQ_SLOTS;
            s_count = 0;
        }
    } else if (s_count > FQ_LATE_DEPTH) {
        jump_to_newest_key();
    }

    if (s_count == 0) {
        LightLock_Unlock(&s_lock);
        LightEvent_WaitTimeout(&s_event, timeout_ns);
        return NULL;
    }

    s_busy = true;
    FqSlot *slot = &s_slots[s_head];
    LightLock_Unlock(&s_lock);
    return slot;
}

void fq_release(void)
{
    LightLock_Lock(&s_lock);
    s_head = (s_head + 1) % FQ_SLOTS;
    s_count--;
    s_busy = false;
    LightLock_Unlock(&s_lock);
}

u32 fq_depth(void)
{
    LightLock_Lock(&s_lock);
    u32 depth = s_count;
    LightLock_Unlock(&s_lock);
    return depth;
}
