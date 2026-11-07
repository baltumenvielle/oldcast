#include "ts_demux.h"

#include <string.h>

void ts_demux_init(TsDemux *d, TsFrameBegin begin, TsFrameEnd end, void *user)
{
    memset(d, 0, sizeof(*d));
    d->begin = begin;
    d->end = end;
    d->user = user;
    d->pmt_pid = -1;
    d->video_pid = -1;
    d->last_cc = -1;
}

// Returns the start of the PSI section pointed to by pointer_field, or NULL.
static const uint8_t *psi_section(const uint8_t *payload, size_t size, size_t *out_size)
{
    if (size < 1 || (size_t)payload[0] + 1 >= size)
        return NULL;
    *out_size = size - 1 - payload[0];
    return payload + 1 + payload[0];
}

static void parse_pat(TsDemux *d, const uint8_t *payload, size_t size)
{
    size_t n;
    const uint8_t *s = psi_section(payload, size, &n);
    if (!s || n < 8 || s[0] != 0x00)
        return;

    size_t section_len = ((s[1] & 0x0F) << 8) | s[2];
    size_t end = section_len + 3; // includes the 4-byte CRC
    if (end > n)
        end = n;
    for (size_t i = 8; i + 4 + 4 <= end; i += 4) {
        int program = (s[i] << 8) | s[i + 1];
        if (program != 0) {
            d->pmt_pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
            return;
        }
    }
}

static void parse_pmt(TsDemux *d, const uint8_t *payload, size_t size)
{
    size_t n;
    const uint8_t *s = psi_section(payload, size, &n);
    if (!s || n < 12 || s[0] != 0x02)
        return;

    size_t section_len = ((s[1] & 0x0F) << 8) | s[2];
    size_t end = section_len + 3;
    if (end > n)
        end = n;
    size_t i = 12 + (((s[10] & 0x0F) << 8) | s[11]);
    while (i + 5 + 4 <= end) {
        int type = s[i];
        int pid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
        if (type == 0x01 || type == 0x02) { // MPEG-1 / MPEG-2 video
            d->video_pid = pid;
            return;
        }
        i += 5 + (((s[i + 3] & 0x0F) << 8) | s[i + 4]);
    }
}

static void frame_finish(TsDemux *d)
{
    if (!d->in_frame)
        return;
    d->in_frame = false;
    d->end(d->user, d->overflow ? 0 : d->len, d->corrupt);
}

static void frame_append(TsDemux *d, const uint8_t *data, size_t size)
{
    if (d->remaining >= 0 && size > (size_t)d->remaining)
        size = (size_t)d->remaining;

    if (d->len + size > d->cap) {
        if (!d->overflow)
            d->overflows++;
        d->overflow = true;
    } else {
        memcpy(d->buf + d->len, data, size);
        d->len += size;
    }

    if (d->remaining >= 0) {
        d->remaining -= (int32_t)size;
        // With a known PES length the frame is delivered as soon as its last byte
        // arrives, instead of when the next frame starts (33 ms later).
        if (d->remaining == 0)
            frame_finish(d);
    }
}

static void pes_start(TsDemux *d, const uint8_t *p, size_t size)
{
    if (d->in_frame) {
        if (d->remaining > 0)
            d->corrupt = true; // the end of the previous frame was lost
        frame_finish(d);
    }

    if (size < 9 || p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01)
        return;
    size_t header = 9 + (size_t)p[8];
    if (header > size)
        return;

    d->buf = d->begin(d->user, &d->cap);
    if (!d->buf)
        return;
    d->len = 0;
    d->corrupt = false;
    d->overflow = false;
    d->in_frame = true;

    size_t pes_len = ((size_t)p[4] << 8) | p[5];
    if (pes_len == 0) {
        d->remaining = -1;
        d->no_length++;
    } else {
        d->remaining = (int32_t)pes_len - 3 - (int32_t)p[8];
        if (d->remaining < 0)
            d->remaining = 0;
    }
    frame_append(d, p + header, size - header);
}

static void feed_packet(TsDemux *d, const uint8_t *p)
{
    d->ts_packets++;
    if (p[0] != 0x47) {
        d->bad_sync++;
        return;
    }

    bool pusi = (p[1] & 0x40) != 0;
    int pid = ((p[1] & 0x1F) << 8) | p[2];
    int afc = (p[3] >> 4) & 3;
    int cc = p[3] & 0x0F;

    if (!(afc & 1))
        return; // no payload: the continuity counter does not advance

    size_t offset = 4;
    if (afc & 2)
        offset += 1 + (size_t)p[4];
    if (offset >= TS_PACKET_SIZE)
        return;
    const uint8_t *payload = p + offset;
    size_t size = TS_PACKET_SIZE - offset;

    if (pid == 0) {
        if (pusi)
            parse_pat(d, payload, size);
    } else if (pid == d->pmt_pid) {
        if (pusi && d->video_pid < 0)
            parse_pmt(d, payload, size);
    } else if (pid == d->video_pid) {
        if (d->last_cc >= 0) {
            if (cc == d->last_cc)
                return; // duplicate packet, allowed by the standard
            int gap = (cc - d->last_cc - 1) & 0x0F;
            if (gap) {
                d->ts_lost += (uint32_t)gap;
                if (d->in_frame)
                    d->corrupt = true;
            }
        }
        d->last_cc = cc;

        if (pusi)
            pes_start(d, payload, size);
        else if (d->in_frame)
            frame_append(d, payload, size);
    }
}

void ts_demux_feed(TsDemux *d, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + TS_PACKET_SIZE <= size; i += TS_PACKET_SIZE)
        feed_packet(d, data + i);
}

int mpeg2_picture_type(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 5 < size; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01 && data[i + 3] == 0x00)
            return (data[i + 5] >> 3) & 7;
    }
    return PIC_UNKNOWN;
}
