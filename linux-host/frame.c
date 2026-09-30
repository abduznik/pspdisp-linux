/* Frame encode: capture (any size) -> scale to PSP 480x272 -> baseline JPEG,
   with optional 90/180/270 rotation. Plus a cheap content hash. */
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>
#include "pspdisp.h"

/* Scale one output pixel from the capture. The host does NOT rotate pixels:
   it only scales the source into the wire image (dims swapped to 272x480 for
   90/270, matching the Windows app's StretchBlt). The PSP performs the actual
   geometric rotation per the rotation flag (psp graphicDrawFrame). Rotating
   here too would double-rotate. */
/* Average up to 3x3 evenly spaced source pixels inside the box that maps to
   this output pixel. Nearest-pixel sampling aliases badly (unreadable text,
   noisy image that also compresses worse). */
#define MAX_TAPS 3
static void sample(capture_backend *cap, int outCol, int outRow, int outW, int outH,
                   uint8_t *rgb)
{
  int x0 = (int)((int64_t)outCol * cap->width  / outW);
  int x1 = (int)((int64_t)(outCol + 1) * cap->width  / outW);
  int y0 = (int)((int64_t)outRow * cap->height / outH);
  int y1 = (int)((int64_t)(outRow + 1) * cap->height / outH);
  int bw = x1 - x0; if (bw < 1) bw = 1;
  int bh = y1 - y0; if (bh < 1) bh = 1;
  int nx = bw < MAX_TAPS ? bw : MAX_TAPS;
  int ny = bh < MAX_TAPS ? bh : MAX_TAPS;
  unsigned sr = 0, sg = 0, sb = 0;
  for (int j = 0; j < ny; j++) {
    int sy = y0 + ((2 * j + 1) * bh) / (2 * ny);
    if (sy >= cap->height) sy = cap->height - 1;
    for (int i = 0; i < nx; i++) {
      int sx = x0 + ((2 * i + 1) * bw) / (2 * nx);
      if (sx >= cap->width) sx = cap->width - 1;
      uint8_t r, g, b;
      cap->pixel(sx, sy, &r, &g, &b);
      sr += r; sg += g; sb += b;
    }
  }
  unsigned n = (unsigned)(nx * ny);
  rgb[0] = sr / n; rgb[1] = sg / n; rgb[2] = sb / n;
}

/* Wire image size presets; must match compressGetFrameSize() in psp/source/compress.c.
   The PSP stretches the image to fill the screen, so aspect need not match. */
static const int scale_presets[4][2] = { {480, 272}, {384, 208}, {320, 176}, {240, 144} };

unsigned char *frame_encode(capture_backend *cap, unsigned long *out_size, uint32_t *out_flags)
{
  bool swap = (g_opt.rotation == 90 || g_opt.rotation == 270);
  int sc = (g_opt.scale >= 0 && g_opt.scale <= 3) ? g_opt.scale : 0;
  int outW = swap ? scale_presets[sc][1] : scale_presets[sc][0];
  int outH = swap ? scale_presets[sc][0] : scale_presets[sc][1];

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  unsigned char *buf = NULL; unsigned long size = 0;
  jpeg_mem_dest(&cinfo, &buf, &size);
  cinfo.image_width = outW; cinfo.image_height = outH;
  cinfo.input_components = 3; cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, g_quality_cur, TRUE);
  /* Eyes barely notice colour detail: quantise chroma coarser than luma. */
  if (g_opt.chroma_boost > 100 && cinfo.quant_tbl_ptrs[1]) {
    for (int i = 0; i < 64; i++) {
      unsigned v = (unsigned)cinfo.quant_tbl_ptrs[1]->quantval[i] * (unsigned)g_opt.chroma_boost / 100;
      cinfo.quant_tbl_ptrs[1]->quantval[i] = (UINT16)(v > 255 ? 255 : (v < 1 ? 1 : v));
    }
  }
  cinfo.optimize_coding = TRUE;
  jpeg_start_compress(&cinfo, TRUE);

  uint8_t *row = malloc((size_t)outW * 3);
  while (cinfo.next_scanline < cinfo.image_height) {
    for (int c = 0; c < outW; c++)
      sample(cap, c, cinfo.next_scanline, outW, outH, &row[c * 3]);
    JSAMPROW r = row;
    jpeg_write_scanlines(&cinfo, &r, 1);
  }
  free(row);
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  uint32_t flags = COM_FLAGS_CONTAINS_IMAGE_DATA | COM_FLAGS_IMAGE_IS_JPEG |
                   ((uint32_t)sc << COM_FLAGS_IMAGE_SCALE_SHIFT);
  if (g_opt.rotation == 90)  flags |= COM_FLAGS_IMAGE_IS_ROTATED_90;
  if (g_opt.rotation == 180) flags |= COM_FLAGS_IMAGE_IS_ROTATED_180;
  if (g_opt.rotation == 270) flags |= COM_FLAGS_IMAGE_IS_ROTATED_270;
  *out_flags = flags;
  *out_size = size;
  return buf;
}

/* FNV-1a over a sparse grid of the capture, for skip-unchanged detection. */
uint64_t frame_hash(capture_backend *cap)
{
  uint64_t h = 1469598103934665603ULL;
  int step_x = cap->width  / 64; if (step_x < 1) step_x = 1;
  int step_y = cap->height / 64; if (step_y < 1) step_y = 1;
  for (int y = 0; y < cap->height; y += step_y)
    for (int x = 0; x < cap->width; x += step_x) {
      uint8_t r, g, b;
      cap->pixel(x, y, &r, &g, &b);
      h = (h ^ r) * 1099511628211ULL;
      h = (h ^ g) * 1099511628211ULL;
      h = (h ^ b) * 1099511628211ULL;
    }
  return h;
}
