#ifndef __BLEND_PS_FUNCTOR_NEON_H__
#define __BLEND_PS_FUNCTOR_NEON_H__

#include "neonutil.h"
#include "blend_functor_c.h"

// PsBlend (Photoshop 互換ブレンド) の NEON 実装。
// SSE2 / AVX2 と概ね同じ「pre_blend hook で ms を変形 → alpha blend」のパターン。
// NEON では vmull_u8 / vmlal_u8 / vshrn_n_u16 / vmvn_u8 が綺麗に揃っているので
// 7bit 量子化のような精度トレードオフは不要、`(s*a + d*(255-a)) >> 8` 8bit 精度で
// byte 演算ができる (`vaddhn_u16` で truncation の高位 narrow add ができる)。
//
// SIMD parity test では PsBlend ファミリ全体に専用トレランス
// (alpha 無視 + tol_rgb=2 / ColorDodge5 のみ tol_rgb=8) を適用しているので、
// NEON byte-exact 実装は誤差 0 で通るはずだが、harness 側を緩めてある分の
// 余裕があるので将来 NEON を最適化する際の精度トレードオフ余地もある。
//
// 現状実装済み: PsAlpha / PsAdd / PsSub / PsMul / PsScreen (5 families × 4 variants)
// 後続コミットで残り 11 families を追加する予定。

// 共通 helper: 1 channel の (d * (255-a) + s * a) >> 8 を u8 で返す。
static inline uint8x8_t
neon_ps_alpha_blend_chan(uint8x8_t d, uint8x8_t s, uint8x8_t a, uint8x8_t a_inv)
{
    // (d * a_inv + s * a) は最大 255*255 + 255*0 = 65025、u16 で収まる。
    // vaddhn_u16(x, y) = (x + y) >> 8 (narrow to u8)
    return vaddhn_u16(vmull_u8(d, a_inv), vmull_u8(s, a));
}

//-------------------------------------
// alpha 抽出 + opa 適用テンプレート
template<typename blend_func>
struct neon_ps_variation : public blend_func
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 a = (s >> 24);
        return blend_func::operator()(d, s, a);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        return blend_func::operator()(md, ms, A_VEC(ms));
    }
};
template<typename blend_func>
struct neon_ps_variation_opa : public blend_func
{
    const tjs_int32 opa_;
    const uint8x8_t opa_vec_;
    inline neon_ps_variation_opa(tjs_int32 opa)
    : opa_(opa)
    , opa_vec_(vdup_n_u8((uint8_t)opa))
    {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 a = ((s >> 24) * opa_) >> 8;
        return blend_func::operator()(d, s, a);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        // a = (sa * opa) >> 8
        uint8x8_t a = vshrn_n_u16(vmull_u8(A_VEC(ms), opa_vec_), 8);
        return blend_func::operator()(md, ms, a);
    }
};

//-------------------------------------
// pre_blend hook 共通の base — alpha blend を行う前に ms を pre_func で変形する
// pre_func: void (*)(uint8x8x4_t& md, uint8x8x4_t& ms) を実装したファンクタ
template<typename pre_func, bool HDA>
struct neon_ps_alpha_blend_base
{
    pre_func pre_;

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a) const
    {
        tjs_uint32 d_orig = d;
        // C ref と同等の per-pixel スカラー処理 (pre_func の scalar 経由)
        s             = pre_.scalar(d, s);
        tjs_uint32 dB = d & 0xff, dG = (d >> 8) & 0xff, dR = (d >> 16) & 0xff;
        tjs_uint32 sB = s & 0xff, sG = (s >> 8) & 0xff, sR = (s >> 16) & 0xff;
        tjs_uint32 ai = 255 - a;
        tjs_uint32 rB = (dB * ai + sB * a) >> 8;
        tjs_uint32 rG = (dG * ai + sG * a) >> 8;
        tjs_uint32 rR = (dR * ai + sR * a) >> 8;
        tjs_uint32 result_a = HDA ? (d_orig & 0xff000000) : 0;
        return result_a | (rR << 16) | (rG << 8) | rB;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms, uint8x8_t ma) const
    {
        uint8x8_t da_save = A_VEC(md);
        pre_(md, ms); // ms に pre_func を適用
        uint8x8_t ma_inv = vmvn_u8(ma);
        B_VEC(md) = neon_ps_alpha_blend_chan(B_VEC(md), B_VEC(ms), ma, ma_inv);
        G_VEC(md) = neon_ps_alpha_blend_chan(G_VEC(md), G_VEC(ms), ma, ma_inv);
        R_VEC(md) = neon_ps_alpha_blend_chan(R_VEC(md), R_VEC(ms), ma, ma_inv);
        if (HDA) {
            A_VEC(md) = da_save; // dst alpha 保持
        } else {
            A_VEC(md) = vdup_n_u8(0); // 非 HDA: alpha clear
        }
        return md;
    }
};

//-------------------------------------
// PsAlphaBlend: pre_blend なし (素のアルファブレンド)
struct neon_ps_alpha_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const { (void)d; return s; }
    inline void operator()(uint8x8x4_t& /*md*/, uint8x8x4_t& /*ms*/) const {}
};
typedef neon_ps_alpha_blend_base<neon_ps_alpha_pre_func, false> neon_ps_alpha_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_alpha_pre_func, true>  neon_ps_alpha_blend_hda;

typedef neon_ps_variation    <neon_ps_alpha_blend_n>     neon_ps_alpha_blend_functor;
typedef neon_ps_variation_opa<neon_ps_alpha_blend_n>     neon_ps_alpha_blend_o_functor;
typedef neon_ps_variation    <neon_ps_alpha_blend_hda>   neon_ps_alpha_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_alpha_blend_hda>   neon_ps_alpha_blend_hda_o_functor;

//-------------------------------------
// PsAddBlend: ms = sat( md + ms ) (linear dodge)
struct neon_ps_add_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto sat = [](tjs_uint32 a, tjs_uint32 b) -> tjs_uint32 {
            return a + b > 255 ? 255 : a + b;
        };
        tjs_uint32 b = sat(d & 0xff, s & 0xff);
        tjs_uint32 g = sat((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = sat((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        B_VEC(ms) = vqadd_u8(B_VEC(md), B_VEC(ms));
        G_VEC(ms) = vqadd_u8(G_VEC(md), G_VEC(ms));
        R_VEC(ms) = vqadd_u8(R_VEC(md), R_VEC(ms));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_add_pre_func, false> neon_ps_add_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_add_pre_func, true>  neon_ps_add_blend_hda;

typedef neon_ps_variation    <neon_ps_add_blend_n>     neon_ps_add_blend_functor;
typedef neon_ps_variation_opa<neon_ps_add_blend_n>     neon_ps_add_blend_o_functor;
typedef neon_ps_variation    <neon_ps_add_blend_hda>   neon_ps_add_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_add_blend_hda>   neon_ps_add_blend_hda_o_functor;

//-------------------------------------
// PsSubBlend (linear burn): ms = sat( md - (255 - ms) ) = max(0, md+ms-255)
struct neon_ps_sub_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto sub = [](tjs_uint32 a, tjs_uint32 b) -> tjs_uint32 {
            return a > b ? a - b : 0;
        };
        tjs_uint32 b = sub(d & 0xff, 255 - (s & 0xff));
        tjs_uint32 g = sub((d >> 8) & 0xff, 255 - ((s >> 8) & 0xff));
        tjs_uint32 r = sub((d >> 16) & 0xff, 255 - ((s >> 16) & 0xff));
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        B_VEC(ms) = vqsub_u8(B_VEC(md), vmvn_u8(B_VEC(ms)));
        G_VEC(ms) = vqsub_u8(G_VEC(md), vmvn_u8(G_VEC(ms)));
        R_VEC(ms) = vqsub_u8(R_VEC(md), vmvn_u8(R_VEC(ms)));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_sub_pre_func, false> neon_ps_sub_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_sub_pre_func, true>  neon_ps_sub_blend_hda;

typedef neon_ps_variation    <neon_ps_sub_blend_n>     neon_ps_sub_blend_functor;
typedef neon_ps_variation_opa<neon_ps_sub_blend_n>     neon_ps_sub_blend_o_functor;
typedef neon_ps_variation    <neon_ps_sub_blend_hda>   neon_ps_sub_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_sub_blend_hda>   neon_ps_sub_blend_hda_o_functor;

//-------------------------------------
// PsMulBlend: ms = (md * ms) >> 8
struct neon_ps_mul_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 b = ((d & 0xff) * (s & 0xff)) >> 8;
        tjs_uint32 g = (((d >> 8) & 0xff) * ((s >> 8) & 0xff)) >> 8;
        tjs_uint32 r = (((d >> 16) & 0xff) * ((s >> 16) & 0xff)) >> 8;
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        B_VEC(ms) = vshrn_n_u16(vmull_u8(B_VEC(md), B_VEC(ms)), 8);
        G_VEC(ms) = vshrn_n_u16(vmull_u8(G_VEC(md), G_VEC(ms)), 8);
        R_VEC(ms) = vshrn_n_u16(vmull_u8(R_VEC(md), R_VEC(ms)), 8);
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_mul_pre_func, false> neon_ps_mul_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_mul_pre_func, true>  neon_ps_mul_blend_hda;

typedef neon_ps_variation    <neon_ps_mul_blend_n>     neon_ps_mul_blend_functor;
typedef neon_ps_variation_opa<neon_ps_mul_blend_n>     neon_ps_mul_blend_o_functor;
typedef neon_ps_variation    <neon_ps_mul_blend_hda>   neon_ps_mul_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_mul_blend_hda>   neon_ps_mul_blend_hda_o_functor;

//-------------------------------------
// PsScreenBlend: ms = d + s - (d*s)>>8 = sat255 of that
struct neon_ps_screen_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto sc = [](tjs_uint32 a, tjs_uint32 b) -> tjs_uint32 {
            tjs_uint32 v = a + b - ((a * b) >> 8);
            return v > 255 ? 255 : v;
        };
        tjs_uint32 b = sc(d & 0xff, s & 0xff);
        tjs_uint32 g = sc((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = sc((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        auto blend = [](uint8x8_t a, uint8x8_t b) -> uint8x8_t {
            uint16x8_t prod = vmull_u8(a, b);
            uint8x8_t  pm   = vshrn_n_u16(prod, 8);
            uint16x8_t sum  = vaddl_u8(a, b);
            return vqmovn_u16(vsubq_u16(sum, vmovl_u8(pm)));
        };
        B_VEC(ms) = blend(B_VEC(md), B_VEC(ms));
        G_VEC(ms) = blend(G_VEC(md), G_VEC(ms));
        R_VEC(ms) = blend(R_VEC(md), R_VEC(ms));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_screen_pre_func, false> neon_ps_screen_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_screen_pre_func, true>  neon_ps_screen_blend_hda;

typedef neon_ps_variation    <neon_ps_screen_blend_n>     neon_ps_screen_blend_functor;
typedef neon_ps_variation_opa<neon_ps_screen_blend_n>     neon_ps_screen_blend_o_functor;
typedef neon_ps_variation    <neon_ps_screen_blend_hda>   neon_ps_screen_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_screen_blend_hda>   neon_ps_screen_blend_hda_o_functor;

//-------------------------------------
// PsLightenBlend: ms = max(d, s) per channel
struct neon_ps_lighten_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto mx = [](tjs_uint32 a, tjs_uint32 b) { return a > b ? a : b; };
        tjs_uint32 b = mx(d & 0xff, s & 0xff);
        tjs_uint32 g = mx((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = mx((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        B_VEC(ms) = vmax_u8(B_VEC(md), B_VEC(ms));
        G_VEC(ms) = vmax_u8(G_VEC(md), G_VEC(ms));
        R_VEC(ms) = vmax_u8(R_VEC(md), R_VEC(ms));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_lighten_pre_func, false> neon_ps_lighten_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_lighten_pre_func, true>  neon_ps_lighten_blend_hda;

typedef neon_ps_variation    <neon_ps_lighten_blend_n>     neon_ps_lighten_blend_functor;
typedef neon_ps_variation_opa<neon_ps_lighten_blend_n>     neon_ps_lighten_blend_o_functor;
typedef neon_ps_variation    <neon_ps_lighten_blend_hda>   neon_ps_lighten_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_lighten_blend_hda>   neon_ps_lighten_blend_hda_o_functor;

//-------------------------------------
// PsDarkenBlend: ms = min(d, s) per channel
struct neon_ps_darken_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto mn = [](tjs_uint32 a, tjs_uint32 b) { return a < b ? a : b; };
        tjs_uint32 b = mn(d & 0xff, s & 0xff);
        tjs_uint32 g = mn((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = mn((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        B_VEC(ms) = vmin_u8(B_VEC(md), B_VEC(ms));
        G_VEC(ms) = vmin_u8(G_VEC(md), G_VEC(ms));
        R_VEC(ms) = vmin_u8(R_VEC(md), R_VEC(ms));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_darken_pre_func, false> neon_ps_darken_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_darken_pre_func, true>  neon_ps_darken_blend_hda;

typedef neon_ps_variation    <neon_ps_darken_blend_n>     neon_ps_darken_blend_functor;
typedef neon_ps_variation_opa<neon_ps_darken_blend_n>     neon_ps_darken_blend_o_functor;
typedef neon_ps_variation    <neon_ps_darken_blend_hda>   neon_ps_darken_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_darken_blend_hda>   neon_ps_darken_blend_hda_o_functor;

//-------------------------------------
// PsDiffBlend: ms = |d - s| per channel
struct neon_ps_diff_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto df = [](tjs_uint32 a, tjs_uint32 b) { return a > b ? a - b : b - a; };
        tjs_uint32 bb = df(d & 0xff, s & 0xff);
        tjs_uint32 g  = df((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r  = df((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | bb;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        // |d - s| = max(d,s) - min(d,s) = vqsub_u8(d,s) | vqsub_u8(s,d)
        B_VEC(ms) = vorr_u8(vqsub_u8(B_VEC(md), B_VEC(ms)), vqsub_u8(B_VEC(ms), B_VEC(md)));
        G_VEC(ms) = vorr_u8(vqsub_u8(G_VEC(md), G_VEC(ms)), vqsub_u8(G_VEC(ms), G_VEC(md)));
        R_VEC(ms) = vorr_u8(vqsub_u8(R_VEC(md), R_VEC(ms)), vqsub_u8(R_VEC(ms), R_VEC(md)));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_diff_pre_func, false> neon_ps_diff_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_diff_pre_func, true>  neon_ps_diff_blend_hda;

typedef neon_ps_variation    <neon_ps_diff_blend_n>     neon_ps_diff_blend_functor;
typedef neon_ps_variation_opa<neon_ps_diff_blend_n>     neon_ps_diff_blend_o_functor;
typedef neon_ps_variation    <neon_ps_diff_blend_hda>   neon_ps_diff_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_diff_blend_hda>   neon_ps_diff_blend_hda_o_functor;

//-------------------------------------
// PsExclusionBlend: c = d + s - 2*(d*s)/255 ≈ d + s - (d*s)>>7
// 16bit で計算 → vqsub_u16 (sat) → vqmovn_u16 で u8 に narrow
struct neon_ps_exclusion_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto ex = [](tjs_uint32 a, tjs_uint32 b) -> tjs_uint32 {
            tjs_int32 v = (tjs_int32)a + (tjs_int32)b - (tjs_int32)((a * b * 2) >> 8);
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            return (tjs_uint32)v;
        };
        tjs_uint32 bb = ex(d & 0xff, s & 0xff);
        tjs_uint32 g  = ex((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r  = ex((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | bb;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        auto ex = [](uint8x8_t a, uint8x8_t b) -> uint8x8_t {
            // d + s - 2*d*s/256 = d + s - (d*s)>>7
            // 中間値 (d*s)>>7 は最大 508 で u8 に収まらないため、u16 のまま計算し
            // 最後に vqmovn_u16 で sat narrow する
            uint16x8_t prod = vmull_u8(a, b);              // a*b (u16)
            uint16x8_t pm   = vshrq_n_u16(prod, 7);        // (a*b)>>7 (u16 のまま)
            uint16x8_t sum  = vaddl_u8(a, b);              // a + b (u16)
            return vqmovn_u16(vqsubq_u16(sum, pm));        // sat sub → sat narrow
        };
        B_VEC(ms) = ex(B_VEC(md), B_VEC(ms));
        G_VEC(ms) = ex(G_VEC(md), G_VEC(ms));
        R_VEC(ms) = ex(R_VEC(md), R_VEC(ms));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_exclusion_pre_func, false> neon_ps_exclusion_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_exclusion_pre_func, true>  neon_ps_exclusion_blend_hda;

typedef neon_ps_variation    <neon_ps_exclusion_blend_n>     neon_ps_exclusion_blend_functor;
typedef neon_ps_variation_opa<neon_ps_exclusion_blend_n>     neon_ps_exclusion_blend_o_functor;
typedef neon_ps_variation    <neon_ps_exclusion_blend_hda>   neon_ps_exclusion_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_exclusion_blend_hda>   neon_ps_exclusion_blend_hda_o_functor;

//-------------------------------------
// 共通 helper: per-channel overlay-style 演算
// dark branch  = 2 * d * s / 255  (dst < 128)
// light branch = 255 - 2 * (255-d) * (255-s) / 255  (dst >= 128)
// PhotoShop の Overlay/HardLight はどちらかの値を threshold で select する。
// arg: a, b は per-channel u8 ベクター。dark の選択条件は cmp_lt(threshold, 128)。
static inline uint8x8_t
neon_ps_overlay_chan(uint8x8_t a, uint8x8_t b, uint8x8_t threshold)
{
    // dark = (2 * a * b) / 255 ≈ vshrn_n_u16(vmull_u8(a, b), 7)
    uint8x8_t dark = vshrn_n_u16(vmull_u8(a, b), 7);
    // light = 255 - (2 * (255-a) * (255-b)) / 255 ≈ 255 - dark_inv
    uint8x8_t na   = vmvn_u8(a);
    uint8x8_t nb   = vmvn_u8(b);
    uint8x8_t darki = vshrn_n_u16(vmull_u8(na, nb), 7);
    uint8x8_t light = vsub_u8(vdup_n_u8(255), darki);
    // mask: threshold < 128 ? 0xff : 0x00
    uint8x8_t mask = vclt_u8(threshold, vdup_n_u8(128));
    // bsl: mask ? dark : light
    return vbsl_u8(mask, dark, light);
}

// PsOverlayBlend: dst < 128 → 2*d*s/255、else → 1-2*(1-d)*(1-s)/255
struct neon_ps_overlay_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto ov = [](tjs_uint32 dch, tjs_uint32 sch) -> tjs_uint32 {
            if (dch < 128) {
                tjs_uint32 v = (2 * dch * sch) >> 8;
                return v > 255 ? 255 : v;
            } else {
                tjs_int32 v =
                    (tjs_int32)255 - (tjs_int32)((2 * (255 - dch) * (255 - sch)) >> 8);
                return v < 0 ? 0 : (tjs_uint32)v;
            }
        };
        tjs_uint32 b = ov(d & 0xff, s & 0xff);
        tjs_uint32 g = ov((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = ov((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        // threshold は dst のチャネル
        B_VEC(ms) = neon_ps_overlay_chan(B_VEC(md), B_VEC(ms), B_VEC(md));
        G_VEC(ms) = neon_ps_overlay_chan(G_VEC(md), G_VEC(ms), G_VEC(md));
        R_VEC(ms) = neon_ps_overlay_chan(R_VEC(md), R_VEC(ms), R_VEC(md));
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_overlay_pre_func, false> neon_ps_overlay_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_overlay_pre_func, true>  neon_ps_overlay_blend_hda;

typedef neon_ps_variation    <neon_ps_overlay_blend_n>     neon_ps_overlay_blend_functor;
typedef neon_ps_variation_opa<neon_ps_overlay_blend_n>     neon_ps_overlay_blend_o_functor;
typedef neon_ps_variation    <neon_ps_overlay_blend_hda>   neon_ps_overlay_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_overlay_blend_hda>   neon_ps_overlay_blend_hda_o_functor;

// PsHardLightBlend: Overlay と同じ条件分岐だが threshold が src 側
struct neon_ps_hardlight_pre_func
{
    inline tjs_uint32 scalar(tjs_uint32 d, tjs_uint32 s) const
    {
        auto hl = [](tjs_uint32 dch, tjs_uint32 sch) -> tjs_uint32 {
            if (sch < 128) {
                tjs_uint32 v = (2 * dch * sch) >> 8;
                return v > 255 ? 255 : v;
            } else {
                tjs_int32 v =
                    (tjs_int32)255 - (tjs_int32)((2 * (255 - dch) * (255 - sch)) >> 8);
                return v < 0 ? 0 : (tjs_uint32)v;
            }
        };
        tjs_uint32 b = hl(d & 0xff, s & 0xff);
        tjs_uint32 g = hl((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = hl((d >> 16) & 0xff, (s >> 16) & 0xff);
        return (s & 0xff000000) | (r << 16) | (g << 8) | b;
    }
    inline void operator()(uint8x8x4_t& md, uint8x8x4_t& ms) const
    {
        // threshold は src のチャネル
        uint8x8_t bms = B_VEC(ms);
        uint8x8_t gms = G_VEC(ms);
        uint8x8_t rms = R_VEC(ms);
        B_VEC(ms) = neon_ps_overlay_chan(B_VEC(md), bms, bms);
        G_VEC(ms) = neon_ps_overlay_chan(G_VEC(md), gms, gms);
        R_VEC(ms) = neon_ps_overlay_chan(R_VEC(md), rms, rms);
    }
};
typedef neon_ps_alpha_blend_base<neon_ps_hardlight_pre_func, false> neon_ps_hardlight_blend_n;
typedef neon_ps_alpha_blend_base<neon_ps_hardlight_pre_func, true>  neon_ps_hardlight_blend_hda;

typedef neon_ps_variation    <neon_ps_hardlight_blend_n>     neon_ps_hardlight_blend_functor;
typedef neon_ps_variation_opa<neon_ps_hardlight_blend_n>     neon_ps_hardlight_blend_o_functor;
typedef neon_ps_variation    <neon_ps_hardlight_blend_hda>   neon_ps_hardlight_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_hardlight_blend_hda>   neon_ps_hardlight_blend_hda_o_functor;

//-------------------------------------
// テーブル lookup 系 (SoftLight / ColorDodge / ColorBurn / ColorDodge5)
// SIMD でテーブル参照は基本的にスカラー fallback。8 pixel 分を一旦メモリに書き出し
// てチャネル毎にテーブルを引いて読み戻す pattern (AVX2 と同じ手法)。
template<typename TTable, bool HDA>
struct neon_ps_table_blend
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a) const
    {
        // s' = TTable[s_byte][d_byte] for each channel
        tjs_uint32 sB = TTable::TABLE[s & 0xff][d & 0xff];
        tjs_uint32 sG = TTable::TABLE[(s >> 8) & 0xff][(d >> 8) & 0xff];
        tjs_uint32 sR = TTable::TABLE[(s >> 16) & 0xff][(d >> 16) & 0xff];
        tjs_uint32 dB = d & 0xff, dG = (d >> 8) & 0xff, dR = (d >> 16) & 0xff;
        tjs_uint32 ai = 255 - a;
        tjs_uint32 rB = (dB * ai + sB * a) >> 8;
        tjs_uint32 rG = (dG * ai + sG * a) >> 8;
        tjs_uint32 rR = (dR * ai + sR * a) >> 8;
        tjs_uint32 result_a = HDA ? (d & 0xff000000) : 0;
        return result_a | (rR << 16) | (rG << 8) | rB;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms, uint8x8_t ma) const
    {
        // 8 pixel 分のチャネルバイトを取り出し、テーブルを引いて新しい uint8x8 を作る
        alignas(16) uint8_t bD[8], gD[8], rD[8], bS[8], gS[8], rS[8];
        vst1_u8(bD, B_VEC(md));
        vst1_u8(gD, G_VEC(md));
        vst1_u8(rD, R_VEC(md));
        vst1_u8(bS, B_VEC(ms));
        vst1_u8(gS, G_VEC(ms));
        vst1_u8(rS, R_VEC(ms));
        for (int i = 0; i < 8; ++i) {
            bS[i] = TTable::TABLE[bS[i]][bD[i]];
            gS[i] = TTable::TABLE[gS[i]][gD[i]];
            rS[i] = TTable::TABLE[rS[i]][rD[i]];
        }
        B_VEC(ms) = vld1_u8(bS);
        G_VEC(ms) = vld1_u8(gS);
        R_VEC(ms) = vld1_u8(rS);

        // 標準の alpha blend
        uint8x8_t da_save = A_VEC(md);
        uint8x8_t ma_inv  = vmvn_u8(ma);
        B_VEC(md) = neon_ps_alpha_blend_chan(B_VEC(md), B_VEC(ms), ma, ma_inv);
        G_VEC(md) = neon_ps_alpha_blend_chan(G_VEC(md), G_VEC(ms), ma, ma_inv);
        R_VEC(md) = neon_ps_alpha_blend_chan(R_VEC(md), R_VEC(ms), ma, ma_inv);
        A_VEC(md) = HDA ? da_save : vdup_n_u8(0);
        return md;
    }
};

typedef neon_ps_table_blend<ps_soft_light_table, false> neon_ps_softlight_blend_n;
typedef neon_ps_table_blend<ps_soft_light_table, true>  neon_ps_softlight_blend_hda;
typedef neon_ps_variation    <neon_ps_softlight_blend_n>    neon_ps_softlight_blend_functor;
typedef neon_ps_variation_opa<neon_ps_softlight_blend_n>    neon_ps_softlight_blend_o_functor;
typedef neon_ps_variation    <neon_ps_softlight_blend_hda>  neon_ps_softlight_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_softlight_blend_hda>  neon_ps_softlight_blend_hda_o_functor;

typedef neon_ps_table_blend<ps_color_dodge_table, false> neon_ps_colordodge_blend_n;
typedef neon_ps_table_blend<ps_color_dodge_table, true>  neon_ps_colordodge_blend_hda;
typedef neon_ps_variation    <neon_ps_colordodge_blend_n>    neon_ps_colordodge_blend_functor;
typedef neon_ps_variation_opa<neon_ps_colordodge_blend_n>    neon_ps_colordodge_blend_o_functor;
typedef neon_ps_variation    <neon_ps_colordodge_blend_hda>  neon_ps_colordodge_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_colordodge_blend_hda>  neon_ps_colordodge_blend_hda_o_functor;

typedef neon_ps_table_blend<ps_color_burn_table, false> neon_ps_colorburn_blend_n;
typedef neon_ps_table_blend<ps_color_burn_table, true>  neon_ps_colorburn_blend_hda;
typedef neon_ps_variation    <neon_ps_colorburn_blend_n>    neon_ps_colorburn_blend_functor;
typedef neon_ps_variation_opa<neon_ps_colorburn_blend_n>    neon_ps_colorburn_blend_o_functor;
typedef neon_ps_variation    <neon_ps_colorburn_blend_hda>  neon_ps_colorburn_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_colorburn_blend_hda>  neon_ps_colorburn_blend_hda_o_functor;

//-------------------------------------
// PsColorDodge5Blend (Photoshop 5.x compat): src を a で先に減衰させてから
// テーブル参照する。alpha_blend を経由しない (s' = TTable[(s*a)>>8][d])。
// 通常の neon_ps_alpha_blend_base のパターンに乗らないので独自に実装。
struct neon_ps_colordodge5_blend_n
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a) const
    {
        tjs_uint32 sB = ((s & 0xff) * a) >> 8;
        tjs_uint32 sG = (((s >> 8) & 0xff) * a) >> 8;
        tjs_uint32 sR = (((s >> 16) & 0xff) * a) >> 8;
        tjs_uint32 rB = ps_color_dodge_table::TABLE[sB][d & 0xff];
        tjs_uint32 rG = ps_color_dodge_table::TABLE[sG][(d >> 8) & 0xff];
        tjs_uint32 rR = ps_color_dodge_table::TABLE[sR][(d >> 16) & 0xff];
        return (rR << 16) | (rG << 8) | rB;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms, uint8x8_t ma) const
    {
        // s' = (s * a) >> 8 全チャネル
        uint8x8_t sB = vshrn_n_u16(vmull_u8(B_VEC(ms), ma), 8);
        uint8x8_t sG = vshrn_n_u16(vmull_u8(G_VEC(ms), ma), 8);
        uint8x8_t sR = vshrn_n_u16(vmull_u8(R_VEC(ms), ma), 8);

        alignas(16) uint8_t bD[8], gD[8], rD[8], bS[8], gS[8], rS[8];
        vst1_u8(bD, B_VEC(md));
        vst1_u8(gD, G_VEC(md));
        vst1_u8(rD, R_VEC(md));
        vst1_u8(bS, sB);
        vst1_u8(gS, sG);
        vst1_u8(rS, sR);
        for (int i = 0; i < 8; ++i) {
            bS[i] = ps_color_dodge_table::TABLE[bS[i]][bD[i]];
            gS[i] = ps_color_dodge_table::TABLE[gS[i]][gD[i]];
            rS[i] = ps_color_dodge_table::TABLE[rS[i]][rD[i]];
        }
        B_VEC(md) = vld1_u8(bS);
        G_VEC(md) = vld1_u8(gS);
        R_VEC(md) = vld1_u8(rS);
        A_VEC(md) = vdup_n_u8(0);
        return md;
    }
};
struct neon_ps_colordodge5_blend_hda : public neon_ps_colordodge5_blend_n
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a) const
    {
        return (d & 0xff000000)
             | neon_ps_colordodge5_blend_n::operator()(d, s, a);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms, uint8x8_t ma) const
    {
        uint8x8_t da = A_VEC(md);
        md = neon_ps_colordodge5_blend_n::operator()(md, ms, ma);
        A_VEC(md) = da;
        return md;
    }
};
typedef neon_ps_variation    <neon_ps_colordodge5_blend_n>    neon_ps_colordodge5_blend_functor;
typedef neon_ps_variation_opa<neon_ps_colordodge5_blend_n>    neon_ps_colordodge5_blend_o_functor;
typedef neon_ps_variation    <neon_ps_colordodge5_blend_hda>  neon_ps_colordodge5_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_colordodge5_blend_hda>  neon_ps_colordodge5_blend_hda_o_functor;

//-------------------------------------
// PsDiff5Blend (Photoshop 5.x compat): src を a で減衰してから |s-d|
//   1. s' = (s * a) >> 8
//   2. result = |s' - d|
struct neon_ps_diff5_blend_n
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a) const
    {
        auto df = [](tjs_uint32 ax, tjs_uint32 bx) {
            return ax > bx ? ax - bx : bx - ax;
        };
        tjs_uint32 sB = ((s & 0xff) * a) >> 8;
        tjs_uint32 sG = (((s >> 8) & 0xff) * a) >> 8;
        tjs_uint32 sR = (((s >> 16) & 0xff) * a) >> 8;
        tjs_uint32 b  = df(sB, d & 0xff);
        tjs_uint32 g  = df(sG, (d >> 8) & 0xff);
        tjs_uint32 r  = df(sR, (d >> 16) & 0xff);
        return (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms, uint8x8_t ma) const
    {
        uint8x8_t sB = vshrn_n_u16(vmull_u8(B_VEC(ms), ma), 8);
        uint8x8_t sG = vshrn_n_u16(vmull_u8(G_VEC(ms), ma), 8);
        uint8x8_t sR = vshrn_n_u16(vmull_u8(R_VEC(ms), ma), 8);
        // |s' - d| = vqsub(s',d) | vqsub(d,s')
        B_VEC(md) = vorr_u8(vqsub_u8(sB, B_VEC(md)), vqsub_u8(B_VEC(md), sB));
        G_VEC(md) = vorr_u8(vqsub_u8(sG, G_VEC(md)), vqsub_u8(G_VEC(md), sG));
        R_VEC(md) = vorr_u8(vqsub_u8(sR, R_VEC(md)), vqsub_u8(R_VEC(md), sR));
        A_VEC(md) = vdup_n_u8(0);
        return md;
    }
};
struct neon_ps_diff5_blend_hda : public neon_ps_diff5_blend_n
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a) const
    {
        return (d & 0xff000000) | neon_ps_diff5_blend_n::operator()(d, s, a);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms, uint8x8_t ma) const
    {
        uint8x8_t da = A_VEC(md);
        md = neon_ps_diff5_blend_n::operator()(md, ms, ma);
        A_VEC(md) = da;
        return md;
    }
};
typedef neon_ps_variation    <neon_ps_diff5_blend_n>    neon_ps_diff5_blend_functor;
typedef neon_ps_variation_opa<neon_ps_diff5_blend_n>    neon_ps_diff5_blend_o_functor;
typedef neon_ps_variation    <neon_ps_diff5_blend_hda>  neon_ps_diff5_blend_hda_functor;
typedef neon_ps_variation_opa<neon_ps_diff5_blend_hda>  neon_ps_diff5_blend_hda_o_functor;

//-------------------------------------
// PsSoftLightBlend を残すと family は揃うが、上の table_blend テンプレートで
// すでに softlight_blend_n / _hda が作られている。

#endif // __BLEND_PS_FUNCTOR_NEON_H__
