#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "neonutil.h"

#include "blend_functor_neon.h"
#include "blend_ps_functor_neon.h"
//#include "interpolation_functor_neon.h"

extern "C"
{
    extern tjs_uint32 TVPCPUType;
    extern unsigned char TVPDivTable[256 * 256];
    extern unsigned char TVPOpacityOnOpacityTable[256 * 256];
    extern unsigned char TVPNegativeMulTable[256 * 256];
    extern unsigned char TVPOpacityOnOpacityTable65[65 * 256];
    extern unsigned char TVPNegativeMulTable65[65 * 256];
    extern unsigned char TVPDitherTable_5_6[8][4][2][256];
    extern unsigned char TVPDitherTable_676[3][4][4][256];
    extern unsigned char TVP252DitherPalette[3][256];
    extern tjs_uint32 TVPRecipTable256[256];
    extern tjs_uint16 TVPRecipTable256_16[256];
}

template<typename functor>
static inline void
blend_func_neon(tjs_uint32 *__restrict dest, const tjs_uint32 *__restrict src,
                tjs_int len, const functor &func)
{
    if (len <= 0)
        return;

    tjs_uint32 rem    = (len >> 3) << 3;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        uint8_t *s = (uint8_t *)src;
        uint8_t *d = (uint8_t *)dest;

        NEON_BLEND_PIXEL_PREFETCH(s, d, 64);

        uint8x8x4_t ms = vld4_u8(s);
        uint8x8x4_t md = vld4_u8(d);
        vst4_u8(d, func(md, ms));

        dest += 8;
        src += 8;
    }

    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest, *src);
        dest++;
        src++;
    }
}

template<typename functor>
static void
copy_func_neon(tjs_uint32 *__restrict dest, const tjs_uint32 *__restrict src, tjs_int len)
{
    functor func;
    blend_func_neon<functor>(dest, src, len, func);
}

// src と dest が重複している可能性のあるもの
template<typename functor>
static inline void
overlap_blend_func_neon(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len,
                        const functor &func)
{
    if (len <= 0)
        return;

    const tjs_uint32 *src_end = src + len;
    if (dest > src && dest < src_end) {
        // backward オーバーラップするので後ろからコピー
        tjs_int remain = (len >> 3) << 3; // 8未満の端数カット
        len--;
        while (len >= remain) {
            dest[len] = func(dest[len], src[len]);
            len--;
        }
        while (len >= 0) {
            uint8_t *s = (uint8_t *)&(src[len - 7]);
            uint8_t *d = (uint8_t *)&(dest[len - 7]);

            NEON_BLEND_PIXEL_PREFETCH(s, d, -64);

            uint8x8x4_t ms = vld4_u8(s);
            uint8x8x4_t md = vld4_u8(d);
            vst4_u8(d, func(md, ms));
            len -= 8;
        }
    } else {
        // forward
        blend_func_neon<functor>(dest, src, len, func);
    }
}

template<typename functor>
static void
overlap_copy_func_neon(tjs_uint32 *__restrict dest, const tjs_uint32 *__restrict src,
                       tjs_int len)
{
    functor func;
    overlap_blend_func_neon<functor>(dest, src, len, func);
}

// dest = src1 * src2 となっているもの
template<typename functor>
static inline void
sd_blend_func_neon(tjs_uint32 *dest, const tjs_uint32 *src1, const tjs_uint32 *src2,
                   tjs_int len, const functor &func)
{
    if (len <= 0)
        return;

    tjs_uint32 rem    = (len >> 3) << 3;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        uint8_t *s1 = (uint8_t *)src1;
        uint8_t *s2 = (uint8_t *)src2;

        NEON_BLEND_PIXEL_PREFETCH(s1, s2, 64);

        uint8x8x4_t ms1 = vld4_u8(s1);
        uint8x8x4_t ms2 = vld4_u8(s2);
        vst4_u8((uint8_t *)dest, func(ms1, ms2));

        dest += 8;
        src1 += 8;
        src2 += 8;
    }

    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*src1, *src2);
        dest++;
        src1++;
        src2++;
    }
}

// 完全透明ではコピーせず、完全不透明はそのままコピーする
template<typename functor>
static void
blend_src_branch_func_neon(tjs_uint32 *__restrict dest, const tjs_uint32 *__restrict src,
                           tjs_int len, const functor &func)
{
    if (len <= 0)
        return;

    tjs_uint32 rem    = (len >> 3) << 3;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        uint8_t *s = (uint8_t *)src;
        uint8_t *d = (uint8_t *)dest;

        NEON_BLEND_PIXEL_PREFETCH(s, d, 64);

        uint8x8x4_t ms = vld4_u8(s);
        uint8x8x4_t md = vld4_u8(d);

        // 分岐処理を行う
        // 単純な neon_alpha_blend の呼び出しではこちらのほうが遅くなるので
        // neon_alpha_blend にはこの分岐版は使っていない。
        // alpha_blend_d 系では、メモリ上のテーブルとのやりとりが発生して
        // コスト高な処理になっているので、この分岐版を経由させている。
#if defined(__aarch64__)
        // vaddlv が ARM64 にしかないので振り分け
        // スカラ値を返すテスト演算子のようなものが NEON にはないので、
        // α値ベクタを水平加算でスカラ値にして、定数比較をする
        static const tjs_uint16 ALL_OPAQUE = 0xff * 8;
        tjs_uint16 a_total                 = vaddlv_u8(A_VEC(ms));
        if (a_total == ALL_OPAQUE) { // 完全不透明
            md = ms;
        } else if (a_total > 0) { // 半透明
            md = func(md, ms);
        } else { // 透明
        }
#else
        // TODO ARM32 はとりあえず全部ブレンド処理を通す
        md = func(md, ms);
#endif
        vst4_u8(d, md);
        dest += 8;
        src += 8;
    }

    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest, *src);
        dest++;
        src++;
    }
}

template<typename functor>
static void
copy_src_branch_func_neon(tjs_uint32 *__restrict dest, const tjs_uint32 *__restrict src,
                          tjs_int len)
{
    functor func;
    blend_src_branch_func_neon<functor>(dest, src, len, func);
}

#define DEFINE_BLEND_FUNCTION_MIN_VARIATION(NAME, FUNC)                                  \
    static void TVP##NAME##_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len) \
    {                                                                                    \
        copy_func_neon<neon_##FUNC##_functor>(dest, src, len);                           \
    }                                                                                    \
    static void TVP##NAME##_HDA_neon_c(tjs_uint32 *dest, const tjs_uint32 *src,          \
                                       tjs_int len)                                      \
    {                                                                                    \
        copy_func_neon<neon_##FUNC##_hda_functor>(dest, src, len);                       \
    }                                                                                    \
    static void TVP##NAME##_o_neon_c(tjs_uint32 *dest, const tjs_uint32 *src,            \
                                     tjs_int len, tjs_int opa)                           \
    {                                                                                    \
        neon_##FUNC##_o_functor func(opa);                                               \
        blend_func_neon(dest, src, len, func);                                           \
    }                                                                                    \
    static void TVP##NAME##_HDA_o_neon_c(tjs_uint32 *dest, const tjs_uint32 *src,        \
                                         tjs_int len, tjs_int opa)                       \
    {                                                                                    \
        neon_##FUNC##_hda_o_functor func(opa);                                           \
        blend_func_neon(dest, src, len, func);                                           \
    }

#define DEFINE_BLEND_FUNCTION_MIN3_VARIATION(NAME, FUNC)                                 \
    static void TVP##NAME##_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len) \
    {                                                                                    \
        copy_func_neon<neon_##FUNC##_functor>(dest, src, len);                           \
    }                                                                                    \
    static void TVP##NAME##_HDA_neon_c(tjs_uint32 *dest, const tjs_uint32 *src,          \
                                       tjs_int len)                                      \
    {                                                                                    \
        copy_func_neon<neon_##FUNC##_hda_functor>(dest, src, len);                       \
    }                                                                                    \
    static void TVP##NAME##_o_neon_c(tjs_uint32 *dest, const tjs_uint32 *src,            \
                                     tjs_int len, tjs_int opa)                           \
    {                                                                                    \
        neon_##FUNC##_o_functor func(opa);                                               \
        blend_func_neon(dest, src, len, func);                                           \
    }

#define DEFINE_BLEND_FUNCTION_MIN2_VARIATION(NAME, FUNC)                                 \
    static void TVP##NAME##_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len) \
    {                                                                                    \
        copy_func_neon<neon_##FUNC##_functor>(dest, src, len);                           \
    }                                                                                    \
    static void TVP##NAME##_HDA_neon_c(tjs_uint32 *dest, const tjs_uint32 *src,          \
                                       tjs_int len)                                      \
    {                                                                                    \
        copy_func_neon<neon_##FUNC##_hda_functor>(dest, src, len);                       \
    }

// AlphaBlendはソースが完全透明/不透明で分岐する特殊版を使うので、個別に書く
static void
TVPAlphaBlend_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    // 現実装では単純なαでは分岐処理が逆効果なので単純に呼び出す
    copy_func_neon<neon_alpha_blend_functor>(dest, src, len);
    // copy_src_branch_func_neon<neon_alpha_blend_functor>(dest, src, len);
}
static void
TVPAlphaBlend_HDA_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    copy_func_neon<neon_alpha_blend_hda_functor>(dest, src, len);
}
static void
TVPAlphaBlend_o_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len, tjs_int opa)
{
    neon_alpha_blend_o_functor func(opa);
    blend_func_neon(dest, src, len, func);
}
static void
TVPAlphaBlend_HDA_o_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len,
                           tjs_int opa)
{
    neon_alpha_blend_hda_o_functor func(opa);
    blend_func_neon(dest, src, len, func);
}
static void
TVPAlphaBlend_d_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    copy_src_branch_func_neon<neon_alpha_blend_d_functor>(dest, src, len);
}
static void
TVPConstAlphaBlend_SD_neon_c(tjs_uint32 *dest, const tjs_uint32 *src1,
                             const tjs_uint32 *src2, tjs_int len, tjs_int opa)
{
    neon_const_alpha_blend_functor func(opa);
    sd_blend_func_neon(dest, src1, src2, len, func);
}
static void
TVPCopyColor_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    overlap_copy_func_neon<neon_color_copy_functor>(dest, src, len);
}
static void
TVPCopyMask_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    overlap_copy_func_neon<neon_alpha_copy_functor>(dest, src, len);
}
static void
TVPCopyOpaqueImage_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    copy_func_neon<neon_color_opaque_functor>(dest, src, len);
}

static void
TVPConstAlphaBlend_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len,
                          tjs_int opa)
{
    neon_const_alpha_blend_functor func(opa);
    blend_func_neon(dest, src, len, func);
}
static void
TVPConstAlphaBlend_HDA_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len,
                              tjs_int opa)
{
    neon_const_alpha_blend_hda_functor func(opa);
    blend_func_neon(dest, src, len, func);
}
static void
TVPConstAlphaBlend_d_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len,
                            tjs_int opa)
{
    neon_const_alpha_blend_d_functor func(opa);
    blend_func_neon(dest, src, len, func);
}
static void
TVPConstAlphaBlend_a_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len,
                            tjs_int opa)
{
    neon_const_alpha_blend_a_functor func(opa);
    blend_func_neon(dest, src, len, func);
}

static void
TVPAdditiveAlphaBlend_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    copy_func_neon<neon_premul_alpha_blend_functor>(dest, src, len);
}
static void
TVPAdditiveAlphaBlend_o_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len,
                               tjs_int opa)
{
    neon_premul_alpha_blend_o_functor func(opa);
    blend_func_neon(dest, src, len, func);
}
static void
TVPAdditiveAlphaBlend_HDA_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    copy_func_neon<neon_premul_alpha_blend_hda_functor>(dest, src, len);
}
static void
TVPAdditiveAlphaBlend_a_neon_c(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    copy_func_neon<neon_premul_alpha_blend_a_functor>(dest, src, len);
}
// 加算 / 減算 / 乗算 / Lighten / Darken / Screen の素朴なブレンドファミリ。
// 現状の neon functor は:
//   - add/sub : functor / _hda / _o / _hda_o (_hda_o は _o の typedef alias)
//   - mul/lighten/darken/screen : functor / _hda のみ
// _o 系は SSE2 / AVX2 ではあるが NEON では別 commit でフォローアップ予定。
DEFINE_BLEND_FUNCTION_MIN_VARIATION(AddBlend,     add_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(SubBlend,     sub_blend)
DEFINE_BLEND_FUNCTION_MIN3_VARIATION(MulBlend,     mul_blend)
DEFINE_BLEND_FUNCTION_MIN2_VARIATION(LightenBlend, lighten_blend)
DEFINE_BLEND_FUNCTION_MIN2_VARIATION(DarkenBlend,  darken_blend)
DEFINE_BLEND_FUNCTION_MIN2_VARIATION(ScreenBlend,  screen_blend)

// Phase 1: NEON PsBlend ファミリ — 順次追加中。
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsAlphaBlend,     ps_alpha_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsAddBlend,       ps_add_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsSubBlend,       ps_sub_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsMulBlend,       ps_mul_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsScreenBlend,    ps_screen_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsLightenBlend,     ps_lighten_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsDarkenBlend,      ps_darken_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsDiffBlend,        ps_diff_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsExclusionBlend,   ps_exclusion_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsOverlayBlend,     ps_overlay_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsHardLightBlend,   ps_hardlight_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsSoftLightBlend,   ps_softlight_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsColorDodgeBlend,  ps_colordodge_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsColorBurnBlend,   ps_colorburn_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsColorDodge5Blend, ps_colordodge5_blend)
DEFINE_BLEND_FUNCTION_MIN_VARIATION(PsDiff5Blend,       ps_diff5_blend)

#if 0 // DEV
// 開発中に未実装命令をトラップするためのダミー
static void
TVPUnimpl_trap_neon_c_3(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len)
{
    abort();
}
static void
TVPUnimpl_trap_neon_c_4(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len, tjs_int opa)
{
    abort();
}
#endif

// extern void TVPInitializeResampleNEON();

// adjust_color_neon.cpp で定義。premul alpha 付きガンマ補正の NEON 版。
extern void TVPAdjustGamma_a_neon_c(tjs_uint32 *dest, tjs_int len,
                                    tTVPGLGammaAdjustTempData *param);

// colormap_neon.cpp で定義。ApplyColorMap ファミリ 9 関数 (Phase 2 C5-C6)。
extern void TVPApplyColorMap_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                    tjs_int len, tjs_uint32 color);
extern void TVPApplyColorMap65_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                      tjs_int len, tjs_uint32 color);
extern void TVPApplyColorMap_o_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                      tjs_int len, tjs_uint32 color, tjs_int opa);
extern void TVPApplyColorMap65_o_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                        tjs_int len, tjs_uint32 color, tjs_int opa);
extern void TVPApplyColorMap_a_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                      tjs_int len, tjs_uint32 color);
extern void TVPApplyColorMap65_a_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                        tjs_int len, tjs_uint32 color);
extern void TVPApplyColorMap_ao_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                       tjs_int len, tjs_uint32 color, tjs_int opa);
extern void TVPApplyColorMap65_ao_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                         tjs_int len, tjs_uint32 color, tjs_int opa);
extern void TVPApplyColorMap65_d_neon_c(tjs_uint32 *dest, const tjs_uint8 *src,
                                        tjs_int len, tjs_uint32 color);

// colorfill_neon.cpp で定義。colorfill ファミリ 7 関数 (Phase 2 D3)。
extern void TVPFillARGB_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 value);
extern void TVPFillARGB_NC_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 value);
extern void TVPFillColor_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 color);
extern void TVPFillMask_neon_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 mask);
extern void TVPConstColorAlphaBlend_neon_c(tjs_uint32 *dest, tjs_int len,
                                           tjs_uint32 color, tjs_int opa);
extern void TVPConstColorAlphaBlend_d_neon_c(tjs_uint32 *dest, tjs_int len,
                                             tjs_uint32 color, tjs_int opa);
extern void TVPConstColorAlphaBlend_a_neon_c(tjs_uint32 *dest, tjs_int len,
                                             tjs_uint32 color, tjs_int opa);

// pixelformat_neon.cpp で定義。3 byte BGR → 4 byte BGRA 変換 (Phase 2 E2)。
extern void TVPConvert24BitTo32Bit_neon_c(tjs_uint32 *dest, const tjs_uint8 *buf,
                                          tjs_int len);

void
TVPGL_NEON_Init()
{
    if (TVPCPUType & TVP_CPU_HAS_ARM_NEON || TVPCPUType & TVP_CPU_HAS_ARM64_ASIMD) {
        // AdditiveAlphaBlend (TVPAdditiveAlphaBlend / _o / _HDA / _a) を有効化。
        // 旧 functor は stub (`return ms;`) だったので、blend_functor_neon.h で
        // C ref と同じ `d * (255-sa) >> 8 + s` の本実装に置き換えてある。
        TVPAdditiveAlphaBlend     = TVPAdditiveAlphaBlend_neon_c;
        TVPAdditiveAlphaBlend_o   = TVPAdditiveAlphaBlend_o_neon_c;
        TVPAdditiveAlphaBlend_HDA = TVPAdditiveAlphaBlend_HDA_neon_c;
        TVPAdditiveAlphaBlend_a   = TVPAdditiveAlphaBlend_a_neon_c;
        // TVPAdditiveAlphaBlend_ao

        TVPAlphaBlend     = TVPAlphaBlend_neon_c;
        TVPAlphaBlend_o   = TVPAlphaBlend_o_neon_c;
        TVPAlphaBlend_HDA = TVPAlphaBlend_HDA_neon_c;
        TVPAlphaBlend_d   = TVPAlphaBlend_d_neon_c;
        // TVPAlphaBlend_a
        // TVPAlphaBlend_do
        // TVPAlphaBlend_ao
        TVPConstAlphaBlend     = TVPConstAlphaBlend_neon_c;
        TVPConstAlphaBlend_HDA = TVPConstAlphaBlend_HDA_neon_c;
        TVPConstAlphaBlend_d   = TVPConstAlphaBlend_d_neon_c;
        TVPConstAlphaBlend_a   = TVPConstAlphaBlend_a_neon_c;

        TVPConstAlphaBlend_SD   = TVPConstAlphaBlend_SD_neon_c;
        TVPConstAlphaBlend_SD_a = TVPConstAlphaBlend_SD_neon_c;

        TVPCopyColor       = TVPCopyColor_neon_c;
        TVPCopyMask        = TVPCopyMask_neon_c;
        TVPCopyOpaqueImage = TVPCopyOpaqueImage_neon_c;

        // 素朴なブレンドファミリ
        TVPAddBlend         = TVPAddBlend_neon_c;
        TVPAddBlend_HDA     = TVPAddBlend_HDA_neon_c;
        TVPAddBlend_o       = TVPAddBlend_o_neon_c;
        TVPAddBlend_HDA_o   = TVPAddBlend_HDA_o_neon_c;
        TVPSubBlend         = TVPSubBlend_neon_c;
        TVPSubBlend_HDA     = TVPSubBlend_HDA_neon_c;
        TVPSubBlend_o       = TVPSubBlend_o_neon_c;
        TVPSubBlend_HDA_o   = TVPSubBlend_HDA_o_neon_c;
        TVPMulBlend         = TVPMulBlend_neon_c;
        TVPMulBlend_HDA     = TVPMulBlend_HDA_neon_c;
        TVPMulBlend_o       = TVPMulBlend_o_neon_c;
        TVPLightenBlend     = TVPLightenBlend_neon_c;
        TVPLightenBlend_HDA = TVPLightenBlend_HDA_neon_c;
        TVPDarkenBlend      = TVPDarkenBlend_neon_c;
        TVPDarkenBlend_HDA  = TVPDarkenBlend_HDA_neon_c;
        TVPScreenBlend      = TVPScreenBlend_neon_c;
        TVPScreenBlend_HDA  = TVPScreenBlend_HDA_neon_c;

        // PsBlend ファミリ (Phase 1: 5/16 families × 4 variants = 20 関数)
        TVPPsAlphaBlend         = TVPPsAlphaBlend_neon_c;
        TVPPsAlphaBlend_o       = TVPPsAlphaBlend_o_neon_c;
        TVPPsAlphaBlend_HDA     = TVPPsAlphaBlend_HDA_neon_c;
        TVPPsAlphaBlend_HDA_o   = TVPPsAlphaBlend_HDA_o_neon_c;
        TVPPsAddBlend           = TVPPsAddBlend_neon_c;
        TVPPsAddBlend_o         = TVPPsAddBlend_o_neon_c;
        TVPPsAddBlend_HDA       = TVPPsAddBlend_HDA_neon_c;
        TVPPsAddBlend_HDA_o     = TVPPsAddBlend_HDA_o_neon_c;
        TVPPsSubBlend           = TVPPsSubBlend_neon_c;
        TVPPsSubBlend_o         = TVPPsSubBlend_o_neon_c;
        TVPPsSubBlend_HDA       = TVPPsSubBlend_HDA_neon_c;
        TVPPsSubBlend_HDA_o     = TVPPsSubBlend_HDA_o_neon_c;
        TVPPsMulBlend           = TVPPsMulBlend_neon_c;
        TVPPsMulBlend_o         = TVPPsMulBlend_o_neon_c;
        TVPPsMulBlend_HDA       = TVPPsMulBlend_HDA_neon_c;
        TVPPsMulBlend_HDA_o     = TVPPsMulBlend_HDA_o_neon_c;
        TVPPsScreenBlend        = TVPPsScreenBlend_neon_c;
        TVPPsScreenBlend_o      = TVPPsScreenBlend_o_neon_c;
        TVPPsScreenBlend_HDA    = TVPPsScreenBlend_HDA_neon_c;
        TVPPsScreenBlend_HDA_o  = TVPPsScreenBlend_HDA_o_neon_c;
        TVPPsLightenBlend         = TVPPsLightenBlend_neon_c;
        TVPPsLightenBlend_o       = TVPPsLightenBlend_o_neon_c;
        TVPPsLightenBlend_HDA     = TVPPsLightenBlend_HDA_neon_c;
        TVPPsLightenBlend_HDA_o   = TVPPsLightenBlend_HDA_o_neon_c;
        TVPPsDarkenBlend          = TVPPsDarkenBlend_neon_c;
        TVPPsDarkenBlend_o        = TVPPsDarkenBlend_o_neon_c;
        TVPPsDarkenBlend_HDA      = TVPPsDarkenBlend_HDA_neon_c;
        TVPPsDarkenBlend_HDA_o    = TVPPsDarkenBlend_HDA_o_neon_c;
        TVPPsDiffBlend            = TVPPsDiffBlend_neon_c;
        TVPPsDiffBlend_o          = TVPPsDiffBlend_o_neon_c;
        TVPPsDiffBlend_HDA        = TVPPsDiffBlend_HDA_neon_c;
        TVPPsDiffBlend_HDA_o      = TVPPsDiffBlend_HDA_o_neon_c;
        TVPPsExclusionBlend       = TVPPsExclusionBlend_neon_c;
        TVPPsExclusionBlend_o     = TVPPsExclusionBlend_o_neon_c;
        TVPPsExclusionBlend_HDA   = TVPPsExclusionBlend_HDA_neon_c;
        TVPPsExclusionBlend_HDA_o = TVPPsExclusionBlend_HDA_o_neon_c;
        TVPPsOverlayBlend         = TVPPsOverlayBlend_neon_c;
        TVPPsOverlayBlend_o       = TVPPsOverlayBlend_o_neon_c;
        TVPPsOverlayBlend_HDA     = TVPPsOverlayBlend_HDA_neon_c;
        TVPPsOverlayBlend_HDA_o   = TVPPsOverlayBlend_HDA_o_neon_c;
        TVPPsHardLightBlend       = TVPPsHardLightBlend_neon_c;
        TVPPsHardLightBlend_o     = TVPPsHardLightBlend_o_neon_c;
        TVPPsHardLightBlend_HDA   = TVPPsHardLightBlend_HDA_neon_c;
        TVPPsHardLightBlend_HDA_o = TVPPsHardLightBlend_HDA_o_neon_c;
        TVPPsSoftLightBlend       = TVPPsSoftLightBlend_neon_c;
        TVPPsSoftLightBlend_o     = TVPPsSoftLightBlend_o_neon_c;
        TVPPsSoftLightBlend_HDA   = TVPPsSoftLightBlend_HDA_neon_c;
        TVPPsSoftLightBlend_HDA_o = TVPPsSoftLightBlend_HDA_o_neon_c;
        TVPPsColorDodgeBlend       = TVPPsColorDodgeBlend_neon_c;
        TVPPsColorDodgeBlend_o     = TVPPsColorDodgeBlend_o_neon_c;
        TVPPsColorDodgeBlend_HDA   = TVPPsColorDodgeBlend_HDA_neon_c;
        TVPPsColorDodgeBlend_HDA_o = TVPPsColorDodgeBlend_HDA_o_neon_c;
        TVPPsColorBurnBlend        = TVPPsColorBurnBlend_neon_c;
        TVPPsColorBurnBlend_o      = TVPPsColorBurnBlend_o_neon_c;
        TVPPsColorBurnBlend_HDA    = TVPPsColorBurnBlend_HDA_neon_c;
        TVPPsColorBurnBlend_HDA_o  = TVPPsColorBurnBlend_HDA_o_neon_c;
        TVPPsColorDodge5Blend      = TVPPsColorDodge5Blend_neon_c;
        TVPPsColorDodge5Blend_o    = TVPPsColorDodge5Blend_o_neon_c;
        TVPPsColorDodge5Blend_HDA  = TVPPsColorDodge5Blend_HDA_neon_c;
        TVPPsColorDodge5Blend_HDA_o = TVPPsColorDodge5Blend_HDA_o_neon_c;
        TVPPsDiff5Blend            = TVPPsDiff5Blend_neon_c;
        TVPPsDiff5Blend_o          = TVPPsDiff5Blend_o_neon_c;
        TVPPsDiff5Blend_HDA        = TVPPsDiff5Blend_HDA_neon_c;
        TVPPsDiff5Blend_HDA_o      = TVPPsDiff5Blend_HDA_o_neon_c;

        // Phase 2: adjust_color
        // TVPAdjustGamma (non-alpha) は C 版と大差ないため SIMD 化せず
        //   (SSE2 版もコメントで同様の理由で wiring されていない)。
        // TVPInitGammaAdjustTempData もテーブル init で perf 非クリティカル。
        TVPAdjustGamma_a = TVPAdjustGamma_a_neon_c;

        // Phase 2: colormap (C5-C6, 9 関数)
        TVPApplyColorMap      = TVPApplyColorMap_neon_c;
        TVPApplyColorMap65    = TVPApplyColorMap65_neon_c;
        TVPApplyColorMap_o    = TVPApplyColorMap_o_neon_c;
        TVPApplyColorMap65_o  = TVPApplyColorMap65_o_neon_c;
        TVPApplyColorMap_a    = TVPApplyColorMap_a_neon_c;
        TVPApplyColorMap65_a  = TVPApplyColorMap65_a_neon_c;
        TVPApplyColorMap_ao   = TVPApplyColorMap_ao_neon_c;
        TVPApplyColorMap65_ao = TVPApplyColorMap65_ao_neon_c;
        TVPApplyColorMap65_d  = TVPApplyColorMap65_d_neon_c;

        // Phase 2: colorfill (D3, 7 関数)
        TVPFillARGB                = TVPFillARGB_neon_c;
        TVPFillARGB_NC             = TVPFillARGB_NC_neon_c;
        TVPFillColor               = TVPFillColor_neon_c;
        TVPFillMask                = TVPFillMask_neon_c;
        TVPConstColorAlphaBlend    = TVPConstColorAlphaBlend_neon_c;
        TVPConstColorAlphaBlend_d  = TVPConstColorAlphaBlend_d_neon_c;
        TVPConstColorAlphaBlend_a  = TVPConstColorAlphaBlend_a_neon_c;

        // Phase 2: pixelformat (E2, 1 関数)
        TVPConvert24BitTo32Bit   = TVPConvert24BitTo32Bit_neon_c;
        TVPBLConvert24BitTo32Bit = TVPConvert24BitTo32Bit_neon_c;

#if 0
		// PsBlend ファミリ 16 種は上で全て有効化済み。
		// 以下は今後 LinTrans / UnivTrans / Gamma を実装する際の参考リスト。
		TVPUnivTransBlend = TVPUnivTransBlend_neon_c;
		TVPUnivTransBlend_a = TVPUnivTransBlend_neon_c;
		TVPUnivTransBlend_d = TVPUnivTransBlend_d_neon_c;
		TVPUnivTransBlend_switch = TVPUnivTransBlend_switch_neon_c;
		TVPUnivTransBlend_switch_a = TVPUnivTransBlend_switch_neon_c;
		TVPUnivTransBlend_switch_d = TVPUnivTransBlend_switch_d_neon_c;

		if( TVPCPUType & TVP_CPU_HAS_AVX ) {
			TVPInitGammaAdjustTempData = TVPInitGammaAdjustTempData_neon_c;
		}
		TVPAdjustGamma_a = TVPAdjustGamma_a_neon_c;

		// アフィン変換用
		TVPLinTransAlphaBlend = TVPLinTransAlphaBlend_neon_c;
		TVPLinTransAlphaBlend_HDA = TVPLinTransAlphaBlend_HDA_neon_c;
		TVPLinTransAlphaBlend_o = TVPLinTransAlphaBlend_o_neon_c;
		TVPLinTransAlphaBlend_HDA_o = TVPLinTransAlphaBlend_HDA_o_neon_c;
		TVPLinTransAlphaBlend_d = TVPLinTransAlphaBlend_d_neon_c;
		TVPLinTransAlphaBlend_a = TVPLinTransAlphaBlend_a_neon_c;
		TVPLinTransAdditiveAlphaBlend = TVPLinTransAdditiveAlphaBlend_neon_c;
		TVPLinTransAdditiveAlphaBlend_HDA = TVPLinTransAdditiveAlphaBlend_HDA_neon_c;
		TVPLinTransAdditiveAlphaBlend_o = TVPLinTransAdditiveAlphaBlend_o_neon_c;
		TVPLinTransAdditiveAlphaBlend_a = TVPLinTransAdditiveAlphaBlend_a_neon_c;
		TVPLinTransCopyOpaqueImage = TVPLinTransCopyOpaqueImage_neon_c;
		TVPLinTransCopy = TVPLinTransCopy_neon_c;
		TVPLinTransColorCopy = TVPLinTransColorCopy_neon_c;
		TVPLinTransConstAlphaBlend = TVPLinTransConstAlphaBlend_neon_c;
		TVPLinTransConstAlphaBlend_HDA = TVPLinTransConstAlphaBlend_HDA_neon_c;
		TVPLinTransConstAlphaBlend_d = TVPLinTransConstAlphaBlend_d_neon_c;
		TVPLinTransConstAlphaBlend_a = TVPLinTransConstAlphaBlend_a_neon_c;
		TVPInterpLinTransCopy = TVPInterpLinTransCopy_neon_c;
		TVPInterpLinTransConstAlphaBlend = TVPInterpLinTransConstAlphaBlend_neon_c;
#endif

#if TODO
        // TVPInitializeResampleNEON();
#endif
    }
}
