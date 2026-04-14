// NEON 版 TVPConvert24BitTo32Bit (Phase 2 pixelformat)
//
// 3 byte (BGR) を 4 byte (BGRA, alpha=0xff) に変換する単純な operation。
// NEON には vld3q_u8 / vst4q_u8 があるので SSE2/SSSE3 の手動 shuffle と
// 比べて非常に clean に書ける:
//
//   vld3q_u8(buf) → 48 byte を B/G/R の 3 × 16 byte vector に deinterleave
//   vst4q_u8(dest, {B, G, R, 0xff}) → 16 pixel × 4 byte = 64 byte で書き戻し
//
// 1 iter = 16 pixel = 48 byte 入力 → 64 byte 出力。SSE2/SSSE3 と同じ
// pixel/iter 数だが、intrinsic の自然さから NEON 実装はコードが短く済む。
//
// AVX2 はスキップした (SSSE3 が既に pshufb で pixel-major shuffle を
// 使っているため AVX2 化の gain が薄い。adjust_color / colormap の
// case A 方針と同じ)。

#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "neonutil.h"

void
TVPConvert24BitTo32Bit_neon_c(tjs_uint32 *dest, const tjs_uint8 *buf, tjs_int len)
{
    if (len <= 0)
        return;

    const tjs_int  rem   = (len >> 4) << 4;  // 16 pixel 単位
    tjs_uint32 *const limit = dest + rem;
    const uint8x16_t alpha = vdupq_n_u8(0xff);

    while (dest < limit) {
        uint8x16x3_t bgr = vld3q_u8(buf);
        uint8x16x4_t bgra;
        bgra.val[0] = bgr.val[0];   // B
        bgra.val[1] = bgr.val[1];   // G
        bgra.val[2] = bgr.val[2];   // R
        bgra.val[3] = alpha;        // A = 0xff
        vst4q_u8((uint8_t *)dest, bgra);
        buf  += 48;
        dest += 16;
    }

    // 端数 (0..15 pixel)
    tjs_int tail = len - rem;
    while (tail-- > 0) {
        *dest = 0xff000000u | ((tjs_uint32)buf[2] << 16)
                            | ((tjs_uint32)buf[1] << 8)
                            |  (tjs_uint32)buf[0];
        buf  += 3;
        ++dest;
    }
}
