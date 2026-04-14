// NEON 版 colorfill ファミリ (Phase 2 D3)
//
// SSE2 / AVX2 実装 (colorfill_sse2.cpp / colorfill_avx2.cpp) を 128bit NEON
// に直訳したもの。対象 7 関数:
//   - TVPFillARGB / TVPFillARGB_NC / TVPFillColor / TVPFillMask (単純 fill 系)
//   - TVPConstColorAlphaBlend / _d / _a (単色 blend 系)
//
// 方針:
//   - fill 系は 16 pixel/iter (vst1q_u32 × 4 アンロール)
//   - blend 系は 4 pixel/iter pixel-major (colormap_neon.cpp と同じスタイル)
//   - _NC は NEON に non-temporal store の相当物が無いので通常 store で OK
//   - _d は AVX2 と同じく TVPNegativeMulTable から scalar gather した値を
//     直接 <<24 で alpha 組み立て (SSE2 の xor/mul トリックは使わない)
//   - _a の opa=255 edge case (opa<<24 の signed overflow) は SSE2 / AVX2
//     と同じ bug を再現する (harness 側で skip 済み)

#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "neonutil.h"
#include <arm_neon.h>

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[256 * 256];
extern unsigned char TVPNegativeMulTable[256 * 256];
}

// ----------------------------------------------------------------------------
// Fill 系
// ----------------------------------------------------------------------------

static inline void
neon_fill16(tjs_uint32 *dest, tjs_int len, tjs_uint32 value)
{
    if (len <= 0) return;
    uint32x4_t v = vdupq_n_u32(value);
    tjs_int rem = (len >> 4) << 4;  // 16 pixel 単位
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        vst1q_u32(dest +  0, v);
        vst1q_u32(dest +  4, v);
        vst1q_u32(dest +  8, v);
        vst1q_u32(dest + 12, v);
        dest += 16;
    }
    tjs_int tail = len - rem;
    while (tail-- > 0) {
        *dest++ = value;
    }
}

void
TVPFillARGB_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 value)
{
    neon_fill16(dest, len, value);
}

void
TVPFillARGB_NC_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 value)
{
    // NEON には non-temporal store 相当が無い。通常 store で十分。
    neon_fill16(dest, len, value);
}

// ----------------------------------------------------------------------------
// FillColor / FillMask
// ----------------------------------------------------------------------------

namespace {

struct neon_const_color_copy_functor {
    const tjs_uint32 color32_;
    const uint32x4_t color_;
    const uint32x4_t alphamask_;
    inline neon_const_color_copy_functor(tjs_uint32 color)
        : color32_(color & 0x00ffffffu),
          color_(vdupq_n_u32(color & 0x00ffffffu)),
          alphamask_(vdupq_n_u32(0xff000000u)) {}
    inline tjs_uint32 operator()(tjs_uint32 d) const {
        return (d & 0xff000000u) | color32_;
    }
    inline uint32x4_t operator()(uint32x4_t md) const {
        return vorrq_u32(vandq_u32(md, alphamask_), color_);
    }
};

struct neon_const_alpha_copy_functor {
    const tjs_uint32 alpha32_;
    const uint32x4_t alpha_;
    const uint32x4_t colormask_;
    inline neon_const_alpha_copy_functor(tjs_uint32 mask)
        : alpha32_(mask << 24),
          alpha_(vdupq_n_u32(mask << 24)),
          colormask_(vdupq_n_u32(0x00ffffffu)) {}
    inline tjs_uint32 operator()(tjs_uint32 d) const {
        return (d & 0x00ffffffu) | alpha32_;
    }
    inline uint32x4_t operator()(uint32x4_t md) const {
        return vorrq_u32(vandq_u32(md, colormask_), alpha_);
    }
};

template<typename functor>
static inline void
neon_const_color_copy_unroll(tjs_uint32 *dest, tjs_int len, const functor &func)
{
    if (len <= 0) return;
    tjs_int rem = (len >> 4) << 4;  // 16 pixel 単位アンロール
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        uint32x4_t md0 = vld1q_u32(dest +  0);
        uint32x4_t md1 = vld1q_u32(dest +  4);
        uint32x4_t md2 = vld1q_u32(dest +  8);
        uint32x4_t md3 = vld1q_u32(dest + 12);
        vst1q_u32(dest +  0, func(md0));
        vst1q_u32(dest +  4, func(md1));
        vst1q_u32(dest +  8, func(md2));
        vst1q_u32(dest + 12, func(md3));
        dest += 16;
    }
    tjs_int tail = len - rem;
    // 4 pixel 単位
    while (tail >= 4) {
        uint32x4_t md = vld1q_u32(dest);
        vst1q_u32(dest, func(md));
        dest += 4;
        tail -= 4;
    }
    while (tail-- > 0) {
        *dest = func(*dest);
        ++dest;
    }
}

} // anonymous namespace

void
TVPFillColor_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 color)
{
    neon_const_color_copy_functor func(color);
    neon_const_color_copy_unroll(dest, len, func);
}

void
TVPFillMask_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 mask)
{
    neon_const_alpha_copy_functor func(mask);
    neon_const_color_copy_unroll(dest, len, func);
}

// ----------------------------------------------------------------------------
// ConstColorAlphaBlend 系
// ----------------------------------------------------------------------------

namespace {

// ConstColorAlphaBlend:  d = (d*(255-opa) + color*opa) >> 8, alpha は保持
struct neon_const_alpha_fill_blend_functor {
    const uint16x8_t invopa_;
    const uint16x8_t color_mul_;  // color_u16 * opa (adds で使う固定項)
    const uint32x4_t alphamask_;
    const uint32x4_t colormask_;

    inline neon_const_alpha_fill_blend_functor(tjs_int32 opa, tjs_int32 color)
        : invopa_(vdupq_n_u16((tjs_uint16)(255 - opa))),
          color_mul_(vmulq_u16(
              vmovl_u8(vreinterpret_u8_u32(vdup_n_u32((tjs_uint32)color))),
              vdupq_n_u16((tjs_uint16)opa))),
          alphamask_(vdupq_n_u32(0xff000000u)),
          colormask_(vdupq_n_u32(0x00ffffffu)) {}

    inline tjs_uint32 operator()(tjs_uint32 d) const {
        tjs_uint32 da = d & 0xff000000u;
        tjs_int invopa = vgetq_lane_u16(invopa_, 0);
        // color_mul_ は 2 pixel broadcast: lane 0..3 = B,G,R,A (1st px)
        tjs_uint32 cb = vgetq_lane_u16(color_mul_, 0);
        tjs_uint32 cg = vgetq_lane_u16(color_mul_, 1);
        tjs_uint32 cr = vgetq_lane_u16(color_mul_, 2);
        tjs_uint32 db = d & 0xff;
        tjs_uint32 dg = (d >> 8) & 0xff;
        tjs_uint32 dr = (d >> 16) & 0xff;
        tjs_uint32 b = (db * invopa + cb);
        tjs_uint32 g = (dg * invopa + cg);
        tjs_uint32 r = (dr * invopa + cr);
        if (b > 0xffff) b = 0xffff;
        if (g > 0xffff) g = 0xffff;
        if (r > 0xffff) r = 0xffff;
        b >>= 8; g >>= 8; r >>= 8;
        return da | b | (g << 8) | (r << 16);
    }

    inline uint32x4_t operator()(uint32x4_t md32) const {
        uint8x16_t md8 = vreinterpretq_u8_u32(md32);
        uint32x4_t ma  = vandq_u32(md32, alphamask_);

        uint16x8_t md_lo = vmovl_u8(vget_low_u8(md8));
        uint16x8_t md_hi = vmovl_u8(vget_high_u8(md8));

        md_lo = vmulq_u16(md_lo, invopa_);
        md_lo = vqaddq_u16(md_lo, color_mul_);
        md_lo = vshrq_n_u16(md_lo, 8);

        md_hi = vmulq_u16(md_hi, invopa_);
        md_hi = vqaddq_u16(md_hi, color_mul_);
        md_hi = vshrq_n_u16(md_hi, 8);

        uint8x16_t packed = vcombine_u8(
            vqmovun_s16(vreinterpretq_s16_u16(md_lo)),
            vqmovun_s16(vreinterpretq_s16_u16(md_hi)));
        uint32x4_t rgb = vandq_u32(vreinterpretq_u32_u8(packed), colormask_);
        return vorrq_u32(rgb, ma);
    }
};

// ConstColorAlphaBlend_d (destructive alpha 更新)
//
// AVX2 と同じ方針: TVPOpacityOnOpacityTable / TVPNegativeMulTable を
// scalar gather して RGB は NEON で blend、alpha は直接組み立てる。
struct neon_const_alpha_fill_blend_d_functor {
    const tjs_int32 opa32_;    // opa << 8
    const uint16x8_t color_;   // 2 pixel 分 u16 展開 (non-HDA, 0 B G R 0 B G R)
    const uint32x4_t colormask_;

    inline neon_const_alpha_fill_blend_d_functor(tjs_int32 opa, tjs_int32 color)
        : opa32_(opa << 8),
          color_(vmovl_u8(vreinterpret_u8_u32(
              vdup_n_u32((tjs_uint32)(color & 0x00ffffff))))),
          colormask_(vdupq_n_u32(0x00ffffffu)) {}

    inline tjs_uint32 operator()(tjs_uint32 d) const {
        tjs_uint32 addr = opa32_ + (d >> 24);
        tjs_uint32 sopa = TVPOpacityOnOpacityTable[addr];
        tjs_uint32 cb = vgetq_lane_u16(color_, 0);
        tjs_uint32 cg = vgetq_lane_u16(color_, 1);
        tjs_uint32 cr = vgetq_lane_u16(color_, 2);
        tjs_int db = d & 0xff;
        tjs_int dg = (d >> 8) & 0xff;
        tjs_int dr = (d >> 16) & 0xff;
        tjs_int b = ((db << 8) + ((tjs_int)cb - db) * (tjs_int)sopa) >> 8;
        tjs_int g = ((dg << 8) + ((tjs_int)cg - dg) * (tjs_int)sopa) >> 8;
        tjs_int r = ((dr << 8) + ((tjs_int)cr - dr) * (tjs_int)sopa) >> 8;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        tjs_uint32 ret = (tjs_uint32)b | ((tjs_uint32)g << 8) | ((tjs_uint32)r << 16);
        tjs_uint32 dopa = (tjs_uint32)TVPNegativeMulTable[addr] << 24;
        return ret | dopa;
    }

    inline uint32x4_t operator()(uint32x4_t md32) const {
        // 4 pixel の alpha から addr を作り scalar gather
        alignas(16) tjs_uint32 d_arr[4];
        vst1q_u32(d_arr, md32);

        tjs_uint32 addr0 = (tjs_uint32)opa32_ + (d_arr[0] >> 24);
        tjs_uint32 addr1 = (tjs_uint32)opa32_ + (d_arr[1] >> 24);
        tjs_uint32 addr2 = (tjs_uint32)opa32_ + (d_arr[2] >> 24);
        tjs_uint32 addr3 = (tjs_uint32)opa32_ + (d_arr[3] >> 24);

        tjs_uint16 s0 = TVPOpacityOnOpacityTable[addr0];
        tjs_uint16 s1 = TVPOpacityOnOpacityTable[addr1];
        tjs_uint16 s2 = TVPOpacityOnOpacityTable[addr2];
        tjs_uint16 s3 = TVPOpacityOnOpacityTable[addr3];
        uint16x8_t mo_lo = vcombine_u16(vdup_n_u16(s0), vdup_n_u16(s1));
        uint16x8_t mo_hi = vcombine_u16(vdup_n_u16(s2), vdup_n_u16(s3));

        uint8x16_t md8 = vreinterpretq_u8_u32(md32);
        uint16x8_t md_lo = vmovl_u8(vget_low_u8(md8));
        uint16x8_t md_hi = vmovl_u8(vget_high_u8(md8));

        // mc = (md << 8 + (color - md) * mo) >> 8  (signed 16bit)
        int16x8_t diff_lo = vreinterpretq_s16_u16(vsubq_u16(color_, md_lo));
        int16x8_t prod_lo = vmulq_s16(diff_lo, vreinterpretq_s16_u16(mo_lo));
        int16x8_t mdsh_lo = vreinterpretq_s16_u16(vshlq_n_u16(md_lo, 8));
        int16x8_t sum_lo  = vaddq_s16(mdsh_lo, prod_lo);
        uint16x8_t out_lo = vshrq_n_u16(vreinterpretq_u16_s16(sum_lo), 8);

        int16x8_t diff_hi = vreinterpretq_s16_u16(vsubq_u16(color_, md_hi));
        int16x8_t prod_hi = vmulq_s16(diff_hi, vreinterpretq_s16_u16(mo_hi));
        int16x8_t mdsh_hi = vreinterpretq_s16_u16(vshlq_n_u16(md_hi, 8));
        int16x8_t sum_hi  = vaddq_s16(mdsh_hi, prod_hi);
        uint16x8_t out_hi = vshrq_n_u16(vreinterpretq_u16_s16(sum_hi), 8);

        uint8x16_t packed = vcombine_u8(vmovn_u16(out_lo), vmovn_u16(out_hi));
        uint32x4_t rgb    = vandq_u32(vreinterpretq_u32_u8(packed), colormask_);

        // dest alpha は TVPNegativeMulTable から直接 scalar gather して <<24
        alignas(16) tjs_uint32 da_arr[4] = {
            (tjs_uint32)TVPNegativeMulTable[addr0] << 24,
            (tjs_uint32)TVPNegativeMulTable[addr1] << 24,
            (tjs_uint32)TVPNegativeMulTable[addr2] << 24,
            (tjs_uint32)TVPNegativeMulTable[addr3] << 24,
        };
        uint32x4_t da = vld1q_u32(da_arr);
        return vorrq_u32(rgb, da);
    }
};

// ConstColorAlphaBlend_a (premul additive alpha)
//
// Di = Di - SaDi + Si   (Di = dest RGB, Si = color RGB × opa, Sa = opa adjusted)
// Da = Da - SaDa + Sa
//
// opa += opa>>7 の adjust + opa=255 edge case (opa<<24 signed overflow)
// は SSE2 / AVX2 と同じ bug を再現する。harness 側で opa=255 は skip 済み。
struct neon_const_alpha_fill_blend_a_functor {
    uint16x8_t mo_;   // 2 pixel 分 broadcast の opa_adj
    uint16x8_t mc_;   // (Si Si Si Sa) × 2

    inline neon_const_alpha_fill_blend_a_functor(tjs_int32 opa, tjs_int32 color) {
        opa += opa >> 7;  // adjust (opa=255 だと 256 になり以下で overflow する)
        mo_ = vdupq_n_u16((tjs_uint16)opa);

        // (color & 0x00ffffff) を 2 pixel 分 u16 展開 → opa を掛けて >>8
        uint32x4_t c32 = vdupq_n_u32((tjs_uint32)color & 0x00ffffffu);
        uint16x8_t c_u16 = vmovl_u8(vget_low_u8(vreinterpretq_u8_u32(c32)));
        uint16x8_t mc = vshrq_n_u16(vmulq_u16(c_u16, mo_), 8);
        // alpha lane (index 3 と 7) を opa_adj に差し替え
        mc = vsetq_lane_u16((tjs_uint16)opa, mc, 3);
        mc = vsetq_lane_u16((tjs_uint16)opa, mc, 7);
        mc_ = mc;
    }

    inline tjs_uint32 operator()(tjs_uint32 d) const {
        tjs_uint32 db = d & 0xff;
        tjs_uint32 dg = (d >> 8) & 0xff;
        tjs_uint32 dr = (d >> 16) & 0xff;
        tjs_uint32 da = (d >> 24) & 0xff;
        tjs_uint32 opa = vgetq_lane_u16(mo_, 0);
        tjs_int b = (tjs_int)db - (tjs_int)((db * opa) >> 8) + (tjs_int)vgetq_lane_u16(mc_, 0);
        tjs_int g = (tjs_int)dg - (tjs_int)((dg * opa) >> 8) + (tjs_int)vgetq_lane_u16(mc_, 1);
        tjs_int r = (tjs_int)dr - (tjs_int)((dr * opa) >> 8) + (tjs_int)vgetq_lane_u16(mc_, 2);
        tjs_int a = (tjs_int)da - (tjs_int)((da * opa) >> 8) + (tjs_int)vgetq_lane_u16(mc_, 3);
        if (b < 0) b = 0; else if (b > 255) b = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (a < 0) a = 0; else if (a > 255) a = 255;
        return (tjs_uint32)b | ((tjs_uint32)g << 8) | ((tjs_uint32)r << 16) | ((tjs_uint32)a << 24);
    }

    inline uint32x4_t operator()(uint32x4_t md32) const {
        uint8x16_t md8 = vreinterpretq_u8_u32(md32);
        uint16x8_t md_lo = vmovl_u8(vget_low_u8(md8));
        uint16x8_t md_hi = vmovl_u8(vget_high_u8(md8));

        // lo: d -= (d*opa)>>8; d += mc
        uint16x8_t mt_lo = vshrq_n_u16(vmulq_u16(md_lo, mo_), 8);
        int16x8_t  r_lo  = vaddq_s16(
            vsubq_s16(vreinterpretq_s16_u16(md_lo), vreinterpretq_s16_u16(mt_lo)),
            vreinterpretq_s16_u16(mc_));

        uint16x8_t mt_hi = vshrq_n_u16(vmulq_u16(md_hi, mo_), 8);
        int16x8_t  r_hi  = vaddq_s16(
            vsubq_s16(vreinterpretq_s16_u16(md_hi), vreinterpretq_s16_u16(mt_hi)),
            vreinterpretq_s16_u16(mc_));

        uint8x16_t packed = vcombine_u8(vqmovun_s16(r_lo), vqmovun_s16(r_hi));
        return vreinterpretq_u32_u8(packed);
    }
};

template<typename functor>
static inline void
neon_const_color_alpha_blend(tjs_uint32 *dest, tjs_int len, const functor &func)
{
    if (len <= 0) return;
    tjs_int rem = (len >> 2) << 2;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        uint32x4_t md = vld1q_u32(dest);
        vst1q_u32(dest, func(md));
        dest += 4;
    }
    tjs_int tail = len - rem;
    while (tail-- > 0) {
        *dest = func(*dest);
        ++dest;
    }
}

} // anonymous namespace

void
TVPConstColorAlphaBlend_neon_c(tjs_uint32 *dest, tjs_int len,
                               tjs_uint32 color, tjs_int opa)
{
    neon_const_alpha_fill_blend_functor func(opa, (tjs_int32)color);
    neon_const_color_alpha_blend(dest, len, func);
}

void
TVPConstColorAlphaBlend_d_neon_c(tjs_uint32 *dest, tjs_int len,
                                 tjs_uint32 color, tjs_int opa)
{
    neon_const_alpha_fill_blend_d_functor func(opa, (tjs_int32)color);
    neon_const_color_alpha_blend(dest, len, func);
}

void
TVPConstColorAlphaBlend_a_neon_c(tjs_uint32 *dest, tjs_int len,
                                 tjs_uint32 color, tjs_int opa)
{
    neon_const_alpha_fill_blend_a_functor func(opa, (tjs_int32)color);
    neon_const_color_alpha_blend(dest, len, func);
}
