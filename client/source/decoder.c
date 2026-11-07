#include "decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/log.h>

static AVCodecContext *s_ctx;
static AVPacket *s_pkt;
static AVFrame *s_frame;

bool dec_init(void)
{
    av_log_set_level(AV_LOG_QUIET);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec)
        return false;
    s_ctx = avcodec_alloc_context3(codec);
    s_pkt = av_packet_alloc();
    s_frame = av_frame_alloc();
    if (!s_ctx || !s_pkt || !s_frame)
        return false;

    // Without LOW_DELAY, mpeg12dec only outputs a frame after decoding the next one.
    s_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_OUTPUT_CORRUPT;
    s_ctx->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_SHOW_ALL;
    s_ctx->err_recognition = 0; // after packet loss, show whatever can be decoded
    s_ctx->thread_count = 1;

    return avcodec_open2(s_ctx, codec, NULL) == 0;
}

void dec_exit(void)
{
    av_frame_free(&s_frame);
    av_packet_free(&s_pkt);
    avcodec_free_context(&s_ctx);
}

const AVFrame *dec_decode(const u8 *data, size_t len)
{
    s_pkt->data = (u8 *)data;
    s_pkt->size = (int)len;
    if (avcodec_send_packet(s_ctx, s_pkt) < 0)
        return NULL;
    if (avcodec_receive_frame(s_ctx, s_frame) < 0)
        return NULL;
    return s_frame;
}
