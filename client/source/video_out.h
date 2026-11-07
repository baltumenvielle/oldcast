/**
 * @file video_out.h
 * @brief YUV->RGB conversion with the Y2R hardware and drawing on the top screen.
 *
 * Y2R writes straight into a GPU texture (8x8 tiled), and the GPU draws it. The GPU
 * also handles the rotated layout of the 3DS framebuffer, so the CPU never touches
 * a pixel. Two textures are alternated so Y2R never writes the one being displayed.
 */
#pragma once

#include <3ds.h>
#include <libavutil/frame.h>
#include <stdbool.h>

#define VO_WIDTH 400
#define VO_HEIGHT 240

bool vo_init(void);
void vo_exit(void);
void vo_clear(void);

/**
 * Shows a status screen on the top screen (menus, connecting, pairing...).
 * Any argument can be NULL. The next video frame simply draws over it.
 */
void vo_show_status(const char *title, const char *detail, const char *hint);

/**
 * Converts a decoded picture into the next texture. Blocks until Y2R finishes.
 * @return false if the picture is not 400x240 or the conversion failed.
 */
bool vo_convert(const AVFrame *frame);
/**
 * Draws the last converted picture on the top screen.
 * @param wait_vblank  Wait for the next VBlank before drawing (C3D_FRAME_SYNCDRAW).
 *                     Without it the picture is submitted at once; the display still
 *                     swaps buffers on a VBlank, so there is no tearing.
 */
void vo_present(bool wait_vblank);
