// NEON 版 TVPAdjustGamma_a
//
// Phase 2 第一弾。SSE2 実装 (adjust_color_sse2.cpp) の 4 pixel fast path を
// NEON で 8 pixel fast path に拡張したもの。ただし:
//
//   - 中間 alpha (0 < a < 255) のベクタ化 SIMD 経路は省略した。理由は
//     (1) per-pixel 3 × テーブル参照がスカラ gather に支配され SIMD 化の
//     ゲインが限定的、(2) 素朴な per-pixel scalar fallback でも C ref と
//     byte-exact になるので harness が自動で通る、という 2 点。
//   - 全 8 pixel 完全不透明の場合は 8 × 3 のスカラテーブル参照のみ。
//   - 全 8 pixel 完全透明の場合は何もしない (premul 前提)。
//   - 混在の場合は per-pixel scalar fallback。
//
// armv8 (aarch64) のみ fast path を有効化する。vaddlv_u8 (水平加算)
// が ARM64 専用命令のため。armv7 では既存 NEON コード (blend_function_neon.cpp
// の blend_src_branch_func_neon) と同じ方針でスカラ全体フォールバック。

#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "neonutil.h"

extern "C" {
    extern tjs_uint16 TVPRecipTable256_16[256];
}

namespace {

// C ref と 1:1 対応のスカラ実装。ここを C ref と一致させておけば
// NEON fast path を使わない経路でも byte-exact が保証される。
static inline tjs_uint32
adjust_gamma_a_pixel(tjs_uint32 d, const tTVPGLGammaAdjustTempData *param)
{
    if (d >= 0xff000000) {
        tjs_uint32 ret = param->B[d & 0xff];
        ret |= param->G[(d >> 8) & 0xff] << 8;
        ret |= param->R[(d >> 16) & 0xff] << 16;
        return 0xff000000 | ret;
    } else if (d != 0) {
        tjs_uint32 alpha     = d >> 24;
        tjs_uint32 alpha_adj = alpha + (alpha >> 7);
        tjs_uint32 recip     = TVPRecipTable256_16[alpha];
        tjs_uint32 t, out;

        t = d & 0xff;
        if (t > alpha) out =  (param->B[255] * alpha_adj >> 8) + t - alpha;
        else           out =   param->B[(recip * t) >> 8] * alpha_adj >> 8;

        t = (d >> 8) & 0xff;
        if (t > alpha) out |= ((param->G[255] * alpha_adj >> 8) + t - alpha) << 8;
        else           out |=  (param->G[(recip * t) >> 8] * alpha_adj >> 8) << 8;

        t = (d >> 16) & 0xff;
        if (t > alpha) out |= ((param->R[255] * alpha_adj >> 8) + t - alpha) << 16;
        else           out |=  (param->R[(recip * t) >> 8] * alpha_adj >> 8) << 16;

        return out | (d & 0xff000000);
    } else {
        return 0;
    }
}

} // anonymous namespace

void
TVPAdjustGamma_a_neon_c(tjs_uint32 *dest, tjs_int len, tTVPGLGammaAdjustTempData *param)
{
    if (len <= 0)
        return;

#if defined(__aarch64__)
    // 8 pixel / iter。vld4_u8 で B/G/R/A をデインターリーブし、A 全合計で
    // 完全不透明 / 完全透明 / 混在 を 1 命令で分岐する。
    const tjs_int rem       = (len >> 3) << 3;
    tjs_uint32 *const limit = dest + rem;

    // 完全不透明判定: alpha 8 pixel 合計が 0xff * 8
    static const tjs_uint16 ALL_OPAQUE = 0xff * 8;

    while (dest < limit) {
        uint8_t *d8 = (uint8_t *)dest;
        NEON_BLEND_PIXEL_PREFETCH(d8, d8, 64);
        uint8x8x4_t md = vld4_u8(d8);

        const tjs_uint16 a_total = vaddlv_u8(A_VEC(md));
        if (a_total == ALL_OPAQUE) {
            // 全 8 pixel 完全不透明 — スカラ 3 テーブル参照のみ
            for (int i = 0; i < 8; ++i) {
                tjs_uint32 d   = dest[i];
                tjs_uint32 ret = param->B[d & 0xff];
                ret |= param->G[(d >> 8) & 0xff] << 8;
                ret |= param->R[(d >> 16) & 0xff] << 16;
                dest[i] = 0xff000000 | ret;
            }
        } else if (a_total == 0) {
            // 全 8 pixel 完全透明 (premul なので全 0) — 何もしない
        } else {
            // 混在 — per-pixel scalar fallback (中には opaque / transparent /
            // 中間が混じっている)
            dest[0] = adjust_gamma_a_pixel(dest[0], param);
            dest[1] = adjust_gamma_a_pixel(dest[1], param);
            dest[2] = adjust_gamma_a_pixel(dest[2], param);
            dest[3] = adjust_gamma_a_pixel(dest[3], param);
            dest[4] = adjust_gamma_a_pixel(dest[4], param);
            dest[5] = adjust_gamma_a_pixel(dest[5], param);
            dest[6] = adjust_gamma_a_pixel(dest[6], param);
            dest[7] = adjust_gamma_a_pixel(dest[7], param);
        }
        dest += 8;
    }

    // 残り端数 (0..7 pixel)
    tjs_int tail = len - rem;
    while (tail-- > 0) {
        *dest = adjust_gamma_a_pixel(*dest, param);
        ++dest;
    }
#else
    // armv7 フォールバック (水平加算命令がないため全体スカラ)
    while (len-- > 0) {
        *dest = adjust_gamma_a_pixel(*dest, param);
        ++dest;
    }
#endif
}
