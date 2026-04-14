// NEON 版 ApplyColorMap ファミリ (Phase 2 colormap C5-C6)
//
// SSE2 / AVX2 実装 (colormap_sse2.cpp / colormap_avx2.cpp) を 4 pixel/iter
// pixel-major NEON に直訳したもの。レイアウトは SSE2 と 1:1 対応:
//
//   md (4 pixel = uint8x16_t) を vmovl_u8(vget_low/high_u8) で 8 u16 × 2
//   に分割:
//     md_lo = pixels 0,1 の 8 u16
//     md_hi = pixels 2,3 の 8 u16
//
//   mc_ は 2 pixel 分の u16 broadcast (lo/hi 両方で同じ)。
//   mo_lo / mo_hi は 2 pixel 分ずつ vcombine_u16 で (s,s,s,s,t,t,t,t) を構成。
//
// tshift=6 (65) は vshrq_n_s16 + vaddq_s16、tshift=8 は vshrq_n_u16 +
// vaddq_u8 (byte-wrap trick) という 2 種類の丸め方式を SSE2 から受け継ぐ。
// 詳細は colormap_sse2.cpp / colormap_avx2.cpp のコメント参照。
//
// 非 HDA 仕様: base / _o の結果 alpha は 0x00ffffff マスクで 0 にする
// (commit c2fac7e2 の SSE2 修正と同じ方針)。opaque fast path 用の color_
// も alpha=0。
//
// _a / _ao / _d の構造も AVX2 と同じ。_d は alpha 計算を scalar gather した
// TVPNegativeMulTable65 値の <<24 で直接組み立てる (AVX2 C4 と同じ方針 —
// SSE2 版の xor/mul トリックは NEON では使わない)。

#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "neonutil.h"
#include <arm_neon.h>
#include <string.h>

extern "C" {
extern unsigned char TVPNegativeMulTable65[65 * 256];
extern unsigned char TVPOpacityOnOpacityTable65[65 * 256];
}

namespace {

// ----------------------------------------------------------------------------
// helper: 2 pixel 分の opa broadcast を 8 u16 で作る
//   (s,s,s,s, t,t,t,t)
// ----------------------------------------------------------------------------
static inline uint16x8_t
pair_broadcast_u16(tjs_uint16 s0, tjs_uint16 s1)
{
    return vcombine_u16(vdup_n_u16(s0), vdup_n_u16(s1));
}

// ----------------------------------------------------------------------------
// base functor: tshift=6 (65 variant)
// ----------------------------------------------------------------------------
template<int tshift>
struct neon_apply_color_map_xx_functor {
    uint16x8_t mc_;     // 2 pixel 分 broadcast: (0B 0G 0R 0A) × 2
    uint32x4_t color_;  // opaque fast path 用 (alpha=0 マスク済み)
    const uint32x4_t rgb_mask_;

    inline neon_apply_color_map_xx_functor(tjs_uint32 color)
        : rgb_mask_(vdupq_n_u32(0x00ffffffu))
    {
        uint32x4_t c = vdupq_n_u32(color);
        color_ = vandq_u32(c, rgb_mask_);
        mc_ = vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(color)));
    }

    // スカラ 1 pixel 版: C ref と同じ式
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        tjs_int sr = (tjs_int)s;
        tjs_int b  = (d >> 0)  & 0xff;
        tjs_int g  = (d >> 8)  & 0xff;
        tjs_int r  = (d >> 16) & 0xff;
        tjs_int cb = (tjs_int)((vgetq_lane_u16(mc_, 0)));
        tjs_int cg = (tjs_int)((vgetq_lane_u16(mc_, 1)));
        tjs_int cr = (tjs_int)((vgetq_lane_u16(mc_, 2)));
        if (tshift == 6) {
            b += ((cb - b) * sr) >> 6;
            g += ((cg - g) * sr) >> 6;
            r += ((cr - r) * sr) >> 6;
        } else {
            // byte-wrap trick: low byte of ((c-d)*s >> 8) added to d mod 256
            b = (b + (((cb - b) * sr) >> 8)) & 0xff;
            g = (g + (((cg - g) * sr) >> 8)) & 0xff;
            r = (r + (((cr - r) * sr) >> 8)) & 0xff;
        }
        if (b < 0) b = 0; else if (b > 255) b = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        return ((tjs_uint32)b) | ((tjs_uint32)g << 8) | ((tjs_uint32)r << 16);
    }

    // ベクタ 4 pixel 版
    inline uint8x16_t operator()(uint8x16_t md, uint16x8_t mo_lo, uint16x8_t mo_hi) const {
        uint16x8_t md_lo = vmovl_u8(vget_low_u8(md));   // pixels 0,1
        uint16x8_t md_hi = vmovl_u8(vget_high_u8(md));  // pixels 2,3

        uint8x8_t plo, phi;
        if (tshift == 6) {
            // lo: signed (c-d) * mo >> 6
            int16x8_t diff_lo = vreinterpretq_s16_u16(vsubq_u16(mc_, md_lo));
            int16x8_t prod_lo = vmulq_s16(diff_lo, vreinterpretq_s16_u16(mo_lo));
            int16x8_t shr_lo  = vshrq_n_s16(prod_lo, 6);
            int16x8_t sum_lo  = vaddq_s16(vreinterpretq_s16_u16(md_lo), shr_lo);

            int16x8_t diff_hi = vreinterpretq_s16_u16(vsubq_u16(mc_, md_hi));
            int16x8_t prod_hi = vmulq_s16(diff_hi, vreinterpretq_s16_u16(mo_hi));
            int16x8_t shr_hi  = vshrq_n_s16(prod_hi, 6);
            int16x8_t sum_hi  = vaddq_s16(vreinterpretq_s16_u16(md_hi), shr_hi);

            plo = vqmovun_s16(sum_lo);
            phi = vqmovun_s16(sum_hi);
        } else {
            // tshift == 8: byte-wrap trick
            //   shr = (c-d) * mo >> 8 (unsigned), md = md + shr byte-wise
            //   md_u16 のパターン 0D 0D... と shr の 0X 0X... を byte 加算
            //   すると 0((D+X)&0xff) が得られ、上位バイトが 0 のまま残る。
            uint16x8_t prod_lo = vmulq_u16(vsubq_u16(mc_, md_lo), mo_lo);
            uint16x8_t shr_lo  = vshrq_n_u16(prod_lo, 8);
            uint8x16_t md_b_lo = vreinterpretq_u8_u16(md_lo);
            uint8x16_t shr_b_lo= vreinterpretq_u8_u16(shr_lo);
            uint8x16_t sum_b_lo= vaddq_u8(md_b_lo, shr_b_lo);

            uint16x8_t prod_hi = vmulq_u16(vsubq_u16(mc_, md_hi), mo_hi);
            uint16x8_t shr_hi  = vshrq_n_u16(prod_hi, 8);
            uint8x16_t md_b_hi = vreinterpretq_u8_u16(md_hi);
            uint8x16_t shr_b_hi= vreinterpretq_u8_u16(shr_hi);
            uint8x16_t sum_b_hi= vaddq_u8(md_b_hi, shr_b_hi);

            // 上位バイトが 0 の状態なので vmovn_u16 相当で低バイトを取る
            plo = vmovn_u16(vreinterpretq_u16_u8(sum_b_lo));
            phi = vmovn_u16(vreinterpretq_u16_u8(sum_b_hi));
        }
        // 非 HDA: alpha = 0
        uint8x16_t packed = vcombine_u8(plo, phi);
        uint32x4_t result = vandq_u32(vreinterpretq_u32_u8(packed), rgb_mask_);
        return vreinterpretq_u8_u32(result);
    }
};

// ----------------------------------------------------------------------------
// wrapper: straight / _o
// ----------------------------------------------------------------------------
template<typename tbase>
struct neon_apply_color_map_xx_straight_functor : tbase {
    inline neon_apply_color_map_xx_straight_functor(tjs_uint32 color) : tbase(color) {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        return tbase::operator()(d, s);
    }
    inline uint8x16_t operator()(uint8x16_t md, const tjs_uint8 *src) const {
        uint16x8_t mo_lo = pair_broadcast_u16(src[0], src[1]);
        uint16x8_t mo_hi = pair_broadcast_u16(src[2], src[3]);
        return tbase::operator()(md, mo_lo, mo_hi);
    }
};

template<typename tbase>
struct neon_apply_color_map_xx_o_functor : tbase {
    tjs_int opa32_;
    inline neon_apply_color_map_xx_o_functor(tjs_uint32 color, tjs_int opa)
        : tbase(color), opa32_(opa) {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        return tbase::operator()(d, (tjs_uint8)((s * opa32_) >> 8));
    }
    inline uint8x16_t operator()(uint8x16_t md, const tjs_uint8 *src) const {
        tjs_uint8 s0 = (tjs_uint8)((src[0] * opa32_) >> 8);
        tjs_uint8 s1 = (tjs_uint8)((src[1] * opa32_) >> 8);
        tjs_uint8 s2 = (tjs_uint8)((src[2] * opa32_) >> 8);
        tjs_uint8 s3 = (tjs_uint8)((src[3] * opa32_) >> 8);
        uint16x8_t mo_lo = pair_broadcast_u16(s0, s1);
        uint16x8_t mo_hi = pair_broadcast_u16(s2, s3);
        return tbase::operator()(md, mo_lo, mo_hi);
    }
};

typedef neon_apply_color_map_xx_straight_functor< neon_apply_color_map_xx_functor<6> >
        neon_apply_color_map65_functor;
typedef neon_apply_color_map_xx_straight_functor< neon_apply_color_map_xx_functor<8> >
        neon_apply_color_map_functor;
typedef neon_apply_color_map_xx_o_functor< neon_apply_color_map_xx_functor<6> >
        neon_apply_color_map65_o_functor;
typedef neon_apply_color_map_xx_o_functor< neon_apply_color_map_xx_functor<8> >
        neon_apply_color_map_o_functor;

// ----------------------------------------------------------------------------
// _a functor (premul alpha 付き)
//
// mc_ レイアウトは SSE2 と同じ: (0B 0G 0R 0100) × 2 の 8 u16。
// alpha lane の 0x100 は `Sa * 0x100 >> tshift` の出力源。
// ----------------------------------------------------------------------------
template<int tshift>
struct neon_apply_color_map_xx_a_functor {
    uint16x8_t mc_;     // (0B 0G 0R 0100) × 2
    uint32x4_t color_;  // _a は branch 版を使わないので参照されないが一応

    inline neon_apply_color_map_xx_a_functor(tjs_uint32 color) {
        // color & 0x00ffffff を u16 展開し、alpha lane に 0x100 を入れる
        uint16x8_t c = vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(color & 0x00ffffffu)));
        c = vsetq_lane_u16(0x100, c, 3);
        c = vsetq_lane_u16(0x100, c, 7);
        mc_ = c;
        color_ = vdupq_n_u32(color | 0xff000000u);
    }

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        // C ref とは別構造だが、tolerance 内で byte-exact / 近似一致する
        // よう SSE2 と同じ計算式を scalar で写経。
        tjs_int sa = (tjs_int)s;
        tjs_int db = (d >> 0)  & 0xff;
        tjs_int dg = (d >> 8)  & 0xff;
        tjs_int dr = (d >> 16) & 0xff;
        tjs_int da = (d >> 24) & 0xff;
        tjs_int cb = (tjs_int)vgetq_lane_u16(mc_, 0);
        tjs_int cg = (tjs_int)vgetq_lane_u16(mc_, 1);
        tjs_int cr = (tjs_int)vgetq_lane_u16(mc_, 2);
        tjs_int ca = 0x100;
        const int T = tshift;
        tjs_int b = db - ((db * sa) >> T) + ((cb * sa) >> T);
        tjs_int g = dg - ((dg * sa) >> T) + ((cg * sa) >> T);
        tjs_int r = dr - ((dr * sa) >> T) + ((cr * sa) >> T);
        tjs_int a = da - ((da * sa) >> T) + ((ca * sa) >> T);
        if (b < 0) b = 0; else if (b > 255) b = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (a < 0) a = 0; else if (a > 255) a = 255;
        return ((tjs_uint32)b) | ((tjs_uint32)g << 8) |
               ((tjs_uint32)r << 16) | ((tjs_uint32)a << 24);
    }

    inline uint8x16_t operator()(uint8x16_t md, uint16x8_t mo_lo, uint16x8_t mo_hi) const {
        uint16x8_t md_lo = vmovl_u8(vget_low_u8(md));
        uint16x8_t md_hi = vmovl_u8(vget_high_u8(md));

        // lo: ms = mo*mc >> T, mds = md*mo >> T, md = md - mds + ms
        uint16x8_t ms_lo  = vshrq_n_u16(vmulq_u16(mo_lo, mc_),  tshift);
        uint16x8_t mds_lo = vshrq_n_u16(vmulq_u16(md_lo, mo_lo), tshift);
        int16x8_t  r_lo   = vaddq_s16(
            vsubq_s16(vreinterpretq_s16_u16(md_lo), vreinterpretq_s16_u16(mds_lo)),
            vreinterpretq_s16_u16(ms_lo));

        uint16x8_t ms_hi  = vshrq_n_u16(vmulq_u16(mo_hi, mc_),  tshift);
        uint16x8_t mds_hi = vshrq_n_u16(vmulq_u16(md_hi, mo_hi), tshift);
        int16x8_t  r_hi   = vaddq_s16(
            vsubq_s16(vreinterpretq_s16_u16(md_hi), vreinterpretq_s16_u16(mds_hi)),
            vreinterpretq_s16_u16(ms_hi));

        return vcombine_u8(vqmovun_s16(r_lo), vqmovun_s16(r_hi));
    }
};

typedef neon_apply_color_map_xx_straight_functor< neon_apply_color_map_xx_a_functor<6> >
        neon_apply_color_map65_a_functor;
typedef neon_apply_color_map_xx_straight_functor< neon_apply_color_map_xx_a_functor<8> >
        neon_apply_color_map_a_functor;
typedef neon_apply_color_map_xx_o_functor< neon_apply_color_map_xx_a_functor<6> >
        neon_apply_color_map65_ao_functor;
typedef neon_apply_color_map_xx_o_functor< neon_apply_color_map_xx_a_functor<8> >
        neon_apply_color_map_ao_functor;

// ----------------------------------------------------------------------------
// _d functor (destructive, 65 only)
//
// AVX2 C4 と同じ方針: scalar gather で TVPOpacityOnOpacityTable65 /
// TVPNegativeMulTable65 を引き、RGB は NEON で blend、alpha は直接組み立てる。
// tolerance tol_alpha=1 があるため SSE2 の xor/mul トリックは使わない。
// ----------------------------------------------------------------------------
struct neon_apply_color_map65_d_functor {
    uint16x8_t mc_;     // (0B 0G 0R 0FF) × 2 (alpha=255 で mc として保持)
    uint32x4_t color_;
    const uint32x4_t rgb_mask_;

    inline neon_apply_color_map65_d_functor(tjs_uint32 color)
        : rgb_mask_(vdupq_n_u32(0x00ffffffu))
    {
        tjs_uint32 c = color | 0xff000000u;
        color_ = vdupq_n_u32(c);
        mc_ = vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(c)));
    }

    // スカラ版: C ref に合わせる
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        if (s == 0) return d;
        tjs_int addr = (s << 8) | (d >> 24);
        tjs_uint32 dopa = TVPNegativeMulTable65[addr];
        tjs_uint32 o    = TVPOpacityOnOpacityTable65[addr];
        tjs_int ob = (d >> 0)  & 0xff;
        tjs_int og = (d >> 8)  & 0xff;
        tjs_int or_= (d >> 16) & 0xff;
        tjs_int cb = (tjs_int)vgetq_lane_u16(mc_, 0);
        tjs_int cg = (tjs_int)vgetq_lane_u16(mc_, 1);
        tjs_int cr = (tjs_int)vgetq_lane_u16(mc_, 2);
        // (md << 8 + (mc-md)*o) >> 8
        tjs_int b = (((ob << 8) + (cb - ob) * (tjs_int)o) >> 8) & 0xff;
        tjs_int g = (((og << 8) + (cg - og) * (tjs_int)o) >> 8) & 0xff;
        tjs_int r = (((or_<< 8) + (cr - or_)* (tjs_int)o) >> 8) & 0xff;
        return (tjs_uint32)b | ((tjs_uint32)g << 8) | ((tjs_uint32)r << 16) | (dopa << 24);
    }

    inline uint8x16_t operator()(uint8x16_t md, const tjs_uint8 *src) const {
        // 4 pixel の d.alpha と src から addr[4] を作り scalar gather
        alignas(16) tjs_uint32 d_arr[4];
        vst1q_u32(d_arr, vreinterpretq_u32_u8(md));

        tjs_uint32 addr0 = ((tjs_uint32)src[0] << 8) | (d_arr[0] >> 24);
        tjs_uint32 addr1 = ((tjs_uint32)src[1] << 8) | (d_arr[1] >> 24);
        tjs_uint32 addr2 = ((tjs_uint32)src[2] << 8) | (d_arr[2] >> 24);
        tjs_uint32 addr3 = ((tjs_uint32)src[3] << 8) | (d_arr[3] >> 24);

        tjs_uint16 o0 = TVPOpacityOnOpacityTable65[addr0];
        tjs_uint16 o1 = TVPOpacityOnOpacityTable65[addr1];
        tjs_uint16 o2 = TVPOpacityOnOpacityTable65[addr2];
        tjs_uint16 o3 = TVPOpacityOnOpacityTable65[addr3];
        uint16x8_t mo_lo = pair_broadcast_u16(o0, o1);
        uint16x8_t mo_hi = pair_broadcast_u16(o2, o3);

        uint16x8_t md_lo = vmovl_u8(vget_low_u8(md));
        uint16x8_t md_hi = vmovl_u8(vget_high_u8(md));

        // md = (md << 8 + (mc-md)*mo) >> 8 (signed 16bit 演算)
        int16x8_t diff_lo = vreinterpretq_s16_u16(vsubq_u16(mc_, md_lo));
        int16x8_t prod_lo = vmulq_s16(diff_lo, vreinterpretq_s16_u16(mo_lo));
        int16x8_t md_lo_s = vreinterpretq_s16_u16(vshlq_n_u16(md_lo, 8));
        int16x8_t sum_lo  = vaddq_s16(md_lo_s, prod_lo);
        uint16x8_t shr_lo = vshrq_n_u16(vreinterpretq_u16_s16(sum_lo), 8);

        int16x8_t diff_hi = vreinterpretq_s16_u16(vsubq_u16(mc_, md_hi));
        int16x8_t prod_hi = vmulq_s16(diff_hi, vreinterpretq_s16_u16(mo_hi));
        int16x8_t md_hi_s = vreinterpretq_s16_u16(vshlq_n_u16(md_hi, 8));
        int16x8_t sum_hi  = vaddq_s16(md_hi_s, prod_hi);
        uint16x8_t shr_hi = vshrq_n_u16(vreinterpretq_u16_s16(sum_hi), 8);

        uint8x16_t rgb8 = vcombine_u8(vmovn_u16(shr_lo), vmovn_u16(shr_hi));
        uint32x4_t rgb  = vandq_u32(vreinterpretq_u32_u8(rgb8), rgb_mask_);

        // dest alpha は TVPNegativeMulTable65 から直接 scalar gather して <<24
        tjs_uint32 da0 = (tjs_uint32)TVPNegativeMulTable65[addr0] << 24;
        tjs_uint32 da1 = (tjs_uint32)TVPNegativeMulTable65[addr1] << 24;
        tjs_uint32 da2 = (tjs_uint32)TVPNegativeMulTable65[addr2] << 24;
        tjs_uint32 da3 = (tjs_uint32)TVPNegativeMulTable65[addr3] << 24;
        alignas(16) tjs_uint32 da_arr[4] = { da0, da1, da2, da3 };
        uint32x4_t da = vld1q_u32(da_arr);

        return vreinterpretq_u8_u32(vorrq_u32(rgb, da));
    }
};

// ----------------------------------------------------------------------------
// 4 pixel/iter ループ (branch / 非 branch)
// ----------------------------------------------------------------------------
template<typename functor, tjs_uint32 topaque32>
static inline void
apply_color_map_branch_func_neon(tjs_uint32 *dest, const tjs_uint8 *src,
                                 tjs_int len, const functor &func)
{
    if (len <= 0) return;
    tjs_int rem = (len >> 2) << 2;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        tjs_uint32 s4;
        memcpy(&s4, src, 4);
        if (s4 == topaque32) {
            vst1q_u32(dest, func.color_);
        } else if (s4 != 0) {
            uint8x16_t md = vreinterpretq_u8_u32(vld1q_u32(dest));
            uint8x16_t r  = func(md, src);
            vst1q_u32(dest, vreinterpretq_u32_u8(r));
        } // else: 完全透明、何もしない
        dest += 4; src += 4;
    }
    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest, *src);
        ++dest; ++src;
    }
}

template<typename functor>
static inline void
apply_color_map_func_neon(tjs_uint32 *dest, const tjs_uint8 *src,
                          tjs_int len, const functor &func)
{
    if (len <= 0) return;
    tjs_int rem = (len >> 2) << 2;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        uint8x16_t md = vreinterpretq_u8_u32(vld1q_u32(dest));
        uint8x16_t r  = func(md, src);
        vst1q_u32(dest, vreinterpretq_u32_u8(r));
        dest += 4; src += 4;
    }
    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest, *src);
        ++dest; ++src;
    }
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// エクスポート: 9 関数
// ----------------------------------------------------------------------------

// 65 は src byte が 0..64 (topaque=0x40) 前提
static const tjs_uint32 CM65_TOPAQUE_32  = 0x40404040u;
static const tjs_uint32 CM256_TOPAQUE_32 = 0xffffffffu;

void TVPApplyColorMap65_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    neon_apply_color_map65_functor func(color);
    apply_color_map_branch_func_neon<neon_apply_color_map65_functor, CM65_TOPAQUE_32>(
        dest, src, len, func);
}

void TVPApplyColorMap_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    neon_apply_color_map_functor func(color);
    apply_color_map_branch_func_neon<neon_apply_color_map_functor, CM256_TOPAQUE_32>(
        dest, src, len, func);
}

void TVPApplyColorMap65_o_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    neon_apply_color_map65_o_functor func(color, opa);
    apply_color_map_func_neon(dest, src, len, func);
}

void TVPApplyColorMap_o_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    neon_apply_color_map_o_functor func(color, opa);
    apply_color_map_func_neon(dest, src, len, func);
}

void TVPApplyColorMap65_a_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    neon_apply_color_map65_a_functor func(color);
    apply_color_map_func_neon(dest, src, len, func);
}

void TVPApplyColorMap_a_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    neon_apply_color_map_a_functor func(color);
    apply_color_map_func_neon(dest, src, len, func);
}

void TVPApplyColorMap65_ao_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    neon_apply_color_map65_ao_functor func(color, opa);
    apply_color_map_func_neon(dest, src, len, func);
}

void TVPApplyColorMap_ao_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    neon_apply_color_map_ao_functor func(color, opa);
    apply_color_map_func_neon(dest, src, len, func);
}

void TVPApplyColorMap65_d_neon_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    neon_apply_color_map65_d_functor func(color);
    apply_color_map_func_neon(dest, src, len, func);
}
