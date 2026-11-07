#include "video_out.h"

#include <citro2d.h>
#include <string.h>

#define TEX_W 512
#define TEX_H 256
#define BYTES_PER_PIXEL 2

static C3D_RenderTarget *s_top;
static C2D_TextBuf s_text_buf;
static C3D_Tex s_tex[2];
static int s_back;
static Handle s_y2r_event;

// Y2R writes the picture starting at the beginning of the texture, and on the 3DS GPU
// the beginning of texture memory is the top edge (v = 1). Verified on hardware: with
// the opposite assumption the picture came out upside down.
static const Tex3DS_SubTexture s_subtex = {
    .width = VO_WIDTH,
    .height = VO_HEIGHT,
    .left = 0.0f,
    .top = 1.0f,
    .right = (float)VO_WIDTH / TEX_W,
    .bottom = 1.0f - (float)VO_HEIGHT / TEX_H,
};

bool vo_init(void)
{
    // Every glyph of the status screens is one citro2d object, hence the default size.
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) || !C2D_Init(C2D_DEFAULT_MAX_OBJECTS))
        return false;
    C2D_Prepare();
    s_text_buf = C2D_TextBufNew(1024);

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    if (!s_top)
        return false;

    for (int i = 0; i < 2; i++) {
        if (!C3D_TexInit(&s_tex[i], TEX_W, TEX_H, GPU_RGB565))
            return false;
        C3D_TexSetFilter(&s_tex[i], GPU_NEAREST, GPU_NEAREST);
        // No dirty CPU cache lines may later overwrite what Y2R writes.
        memset(s_tex[i].data, 0, s_tex[i].size);
        GSPGPU_FlushDataCache(s_tex[i].data, s_tex[i].size);
    }

    if (R_FAILED(y2rInit()))
        return false;

    Y2RU_ConversionParams params = {
        .input_format = INPUT_YUV420_INDIV_8,
        .output_format = OUTPUT_RGB_16_565,
        .rotation = ROTATION_NONE,
        .block_alignment = BLOCK_8_BY_8, // native texture layout of the PICA200 GPU
        .input_line_width = VO_WIDTH,
        .input_lines = VO_HEIGHT,
        // ffmpeg converts RGB to yuv420p with limited-range BT.601.
        .standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING,
        .alpha = 0xFF,
    };
    if (R_FAILED(Y2RU_SetConversionParams(&params)))
        return false;
    if (R_FAILED(Y2RU_SetTransferEndInterrupt(true)))
        return false;
    if (R_FAILED(Y2RU_GetTransferEndEvent(&s_y2r_event)))
        return false;

    vo_clear();
    return true;
}

void vo_exit(void)
{
    Y2RU_StopConversion();
    y2rExit();
    C3D_TexDelete(&s_tex[0]);
    C3D_TexDelete(&s_tex[1]);
    C2D_TextBufDelete(s_text_buf);
    C2D_Fini();
    C3D_Fini();
}

void vo_clear(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top, C2D_Color32(0, 0, 0, 255));
    C3D_FrameEnd(0);
}

/** Draws one centered line of text with the system font. */
static void draw_centered(const char *str, float y, float scale, u32 color)
{
    if (!str || !*str)
        return;
    C2D_Text text;
    C2D_TextParse(&text, s_text_buf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_AlignCenter | C2D_WithColor, VO_WIDTH / 2.0f, y, 0.5f, scale, scale, color);
}

void vo_show_status(const char *title, const char *detail, const char *hint)
{
    const u32 background = C2D_Color32(0x1f, 0x1d, 0x4f, 0xff);
    const u32 band = C2D_Color32(0x3b, 0x2f, 0x8f, 0xff);
    const u32 white = C2D_Color32(0xf4, 0xf2, 0xff, 0xff);
    const u32 accent = C2D_Color32(0xff, 0x7a, 0x6b, 0xff);
    const u32 muted = C2D_Color32(0xa9, 0xa4, 0xd6, 0xff);

    C2D_TextBufClear(s_text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top, background);
    C2D_SceneBegin(s_top);
    C2D_DrawRectSolid(0, 0, 0.1f, VO_WIDTH, 36, band);
    draw_centered("OldCast", 6, 0.75f, white);
    draw_centered(title, 78, 0.9f, white);
    draw_centered(detail, 122, 0.65f, accent);
    draw_centered(hint, 200, 0.5f, muted);
    C3D_FrameEnd(0);
}

bool vo_convert(const AVFrame *frame)
{
    if (frame->width != VO_WIDTH || frame->height != VO_HEIGHT)
        return false;

    const int w = VO_WIDTH, h = VO_HEIGHT;
    const int cw = w / 2, ch = h / 2;

    // Y2R reads memory through DMA, bypassing the CPU cache.
    GSPGPU_FlushDataCache(frame->data[0], frame->linesize[0] * h);
    GSPGPU_FlushDataCache(frame->data[1], frame->linesize[1] * ch);
    GSPGPU_FlushDataCache(frame->data[2], frame->linesize[2] * ch);

    s_back ^= 1;

    // Transfer 8 lines at a time: with 1-line units, at 400x240 the end-of-transfer
    // interrupt can fire right after the conversion starts (see the note in y2r.h).
    const s16 unit = (s16)(w * 8 * BYTES_PER_PIXEL);
    const s16 gap = (s16)((TEX_W - w) * 8 * BYTES_PER_PIXEL);

    if (R_FAILED(Y2RU_SetSendingY(frame->data[0], w * h, w, frame->linesize[0] - w)) ||
        R_FAILED(Y2RU_SetSendingU(frame->data[1], cw * ch, cw, frame->linesize[1] - cw)) ||
        R_FAILED(Y2RU_SetSendingV(frame->data[2], cw * ch, cw, frame->linesize[2] - cw)) ||
        R_FAILED(Y2RU_SetReceiving(s_tex[s_back].data, w * h * BYTES_PER_PIXEL, unit, gap)) ||
        R_FAILED(Y2RU_StartConversion()))
        return false;

    if (R_FAILED(svcWaitSynchronization(s_y2r_event, 50 * 1000000LL))) {
        Y2RU_StopConversion();
        return false;
    }
    return true;
}

void vo_present(bool wait_vblank)
{
    C2D_Image image = {.tex = &s_tex[s_back], .subtex = &s_subtex};

    C3D_FrameBegin(wait_vblank ? C3D_FRAME_SYNCDRAW : 0);
    C2D_SceneBegin(s_top);
    C2D_DrawImageAt(image, 0.0f, 0.0f, 0.5f, NULL, 1.0f, 1.0f);
    C3D_FrameEnd(0);
}
