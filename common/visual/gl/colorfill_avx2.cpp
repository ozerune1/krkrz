// AVX2 版 colorfill ファミリ (Phase 2 D2)
//
// SSE2 実装 (colorfill_sse2.cpp) を AVX2 256bit で 8 pixel/iter に拡張。
// 対象 7 関数:
//   - TVPFillARGB / TVPFillARGB_NC / TVPFillColor / TVPFillMask (単純 fill 系)
//   - TVPConstColorAlphaBlend / _d / _a (単色 blend 系)
//
// アルゴリズムは SSE2 と 1:1 対応。256bit の unpacklo/hi_epi8 が 128bit
// lane 内で動く特性は ColorMap_avx2 と同じパターンで扱う:
//   md1 (8 pixel = __m256i) → md_lo = {pixels 0,1,4,5}, md_hi = {2,3,6,7}
//
// 非 HDA 仕様や tolerance 対応は SSE2 と同一。SSE2 側で既に確認されて
// いる divergence pattern (_d の scalar tail 丸め / _a の premul 構造差 /
// opa=255 edge case) はそのまま踏襲する。

#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "simd_def_x86x64.h"

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[256 * 256];
extern unsigned char TVPNegativeMulTable[256 * 256];
}

namespace {

// ----------------------------------------------------------------------------
// Fill 系 (TVPFillARGB / NC / Color / Mask)
// ----------------------------------------------------------------------------

// 32 pixel/iter で _mm256_store_si256 4 発アンロール。SSE2 の 16 pixel/iter
// をそのまま倍にした形。
static inline void avx2_fill32(tjs_uint32 *dest, tjs_int len, __m256i mvalue)
{
    if (len <= 0) return;
    // 32 byte アライメント
    tjs_int count = (tjs_int)((uintptr_t)dest & 0x1F);
    if (count) {
        count = (32 - count) >> 2;
        if (count > len) count = len;
        tjs_uint32 val = (tjs_uint32)_mm256_cvtsi256_si32(mvalue);
        tjs_uint32 *limit = dest + count;
        while (dest < limit) {
            *dest++ = val;
        }
        len -= count;
    }
    tjs_int rem       = (len >> 5) << 5;  // 32 pixel ごと
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        _mm256_store_si256((__m256i *)(dest +  0), mvalue);
        _mm256_store_si256((__m256i *)(dest +  8), mvalue);
        _mm256_store_si256((__m256i *)(dest + 16), mvalue);
        _mm256_store_si256((__m256i *)(dest + 24), mvalue);
        dest += 32;
    }
    tjs_uint32 val = (tjs_uint32)_mm256_cvtsi256_si32(mvalue);
    limit += (len - rem);
    while (dest < limit) {
        *dest++ = val;
    }
}

// NC は _mm256_stream_si256 経由 (non-temporal store)
static inline void avx2_fill32_nc(tjs_uint32 *dest, tjs_int len, __m256i mvalue)
{
    if (len <= 0) return;
    tjs_int count = (tjs_int)((uintptr_t)dest & 0x1F);
    if (count) {
        count = (32 - count) >> 2;
        if (count > len) count = len;
        tjs_uint32 val = (tjs_uint32)_mm256_cvtsi256_si32(mvalue);
        tjs_uint32 *limit = dest + count;
        while (dest < limit) {
            *dest++ = val;
        }
        len -= count;
    }
    tjs_int rem       = (len >> 5) << 5;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        _mm256_stream_si256((__m256i *)(dest +  0), mvalue);
        _mm256_stream_si256((__m256i *)(dest +  8), mvalue);
        _mm256_stream_si256((__m256i *)(dest + 16), mvalue);
        _mm256_stream_si256((__m256i *)(dest + 24), mvalue);
        dest += 32;
    }
    tjs_uint32 val = (tjs_uint32)_mm256_cvtsi256_si32(mvalue);
    limit += (len - rem);
    while (dest < limit) {
        *dest++ = val;
    }
}

// ----------------------------------------------------------------------------
// ConstColor / ConstAlpha copy functors (TVPFillColor / TVPFillMask 用)
// ----------------------------------------------------------------------------

struct avx2_const_color_copy_functor {
    const tjs_uint32 color32_;
    const __m256i color_;
    const __m256i alphamask_;
    inline avx2_const_color_copy_functor(tjs_uint32 color) :
        color32_(color & 0x00ffffff),
        color_(_mm256_set1_epi32((int)(color & 0x00ffffff))),
        alphamask_(_mm256_set1_epi32((int)0xff000000)) {}
    inline tjs_uint32 operator()(tjs_uint32 d) const {
        return (d & 0xff000000) + color32_;
    }
    inline __m256i operator()(__m256i md) const {
        return _mm256_or_si256(_mm256_and_si256(md, alphamask_), color_);
    }
};

struct avx2_const_alpha_copy_functor {
    const tjs_uint32 alpha32_;
    const __m256i alpha_;
    const __m256i colormask_;
    inline avx2_const_alpha_copy_functor(tjs_uint32 mask) :
        alpha32_(mask << 24),
        alpha_(_mm256_set1_epi32((int)(mask << 24))),
        colormask_(_mm256_set1_epi32(0x00ffffff)) {}
    inline tjs_uint32 operator()(tjs_uint32 d) const {
        return (d & 0x00ffffff) + alpha32_;
    }
    inline __m256i operator()(__m256i md) const {
        return _mm256_or_si256(_mm256_and_si256(md, colormask_), alpha_);
    }
};

// 16 pixel/iter (SSE2 の 16 と同じ)、4 × __m256i unrolled
template<typename functor>
static inline void avx2_const_color_copy_unroll(
    tjs_uint32 *dest, tjs_int len, const functor &func)
{
    if (len <= 0) return;
    tjs_int count = (tjs_int)((uintptr_t)dest & 0x1F);
    if (count) {
        count = (32 - count) >> 2;
        if (count > len) count = len;
        tjs_uint32 *limit = dest + count;
        while (dest < limit) {
            *dest = func(*dest);
            ++dest;
        }
        len -= count;
    }
    tjs_int rem       = (len >> 5) << 5;  // 32 pixel ごと (8 × 4)
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        __m256i md0 = _mm256_load_si256((const __m256i *)(dest +  0));
        __m256i md1 = _mm256_load_si256((const __m256i *)(dest +  8));
        __m256i md2 = _mm256_load_si256((const __m256i *)(dest + 16));
        __m256i md3 = _mm256_load_si256((const __m256i *)(dest + 24));
        _mm256_store_si256((__m256i *)(dest +  0), func(md0));
        _mm256_store_si256((__m256i *)(dest +  8), func(md1));
        _mm256_store_si256((__m256i *)(dest + 16), func(md2));
        _mm256_store_si256((__m256i *)(dest + 24), func(md3));
        dest += 32;
    }
    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest);
        ++dest;
    }
}

// ----------------------------------------------------------------------------
// ConstColorAlphaBlend (base, HDA 保持)
// ----------------------------------------------------------------------------

struct avx2_const_alpha_fill_blend_functor {
    const __m256i invopa_;      // 255 - opa
    const __m256i zero_;
    __m256i color_;             // color_unpacked * opa
    const __m256i alphamask_;
    const __m256i colormask_;
    inline avx2_const_alpha_fill_blend_functor(tjs_int32 opa, tjs_int32 color) :
        zero_(_mm256_setzero_si256()),
        invopa_(_mm256_set1_epi16((short)(255 - opa))),
        alphamask_(_mm256_set1_epi32((int)0xff000000)),
        colormask_(_mm256_set1_epi32(0x00ffffff))
    {
        __m256i mopa = _mm256_set1_epi16((short)opa);
        __m256i c = _mm256_set1_epi32(color);
        c = _mm256_unpacklo_epi8(c, zero_);  // 各 lane: (0A 0R 0G 0B) × 2
        color_ = _mm256_mullo_epi16(c, mopa);
    }

    inline tjs_uint32 operator()(tjs_uint32 d) const {
        const __m128i zero128 = _mm_setzero_si128();
        __m128i md = _mm_cvtsi32_si128((int)d);
        __m128i ma = md;
        ma = _mm_and_si128(ma, _mm256_castsi256_si128(alphamask_));
        md = _mm_unpacklo_epi8(md, zero128);
        md = _mm_mullo_epi16(md, _mm256_castsi256_si128(invopa_));
        md = _mm_adds_epu16(md, _mm256_castsi256_si128(color_));
        md = _mm_srli_epi16(md, 8);
        md = _mm_packus_epi16(md, zero128);
        md = _mm_and_si128(md, _mm256_castsi256_si128(colormask_));
        md = _mm_or_si128(md, ma);
        return (tjs_uint32)_mm_cvtsi128_si32(md);
    }

    inline __m256i operator()(__m256i md1) const {
        __m256i ma = _mm256_and_si256(md1, alphamask_);
        __m256i md_lo = _mm256_unpacklo_epi8(md1, zero_);
        __m256i md_hi = _mm256_unpackhi_epi8(md1, zero_);

        md_lo = _mm256_mullo_epi16(md_lo, invopa_);
        md_lo = _mm256_adds_epu16(md_lo, color_);
        md_lo = _mm256_srli_epi16(md_lo, 8);

        md_hi = _mm256_mullo_epi16(md_hi, invopa_);
        md_hi = _mm256_adds_epu16(md_hi, color_);
        md_hi = _mm256_srli_epi16(md_hi, 8);

        __m256i packed = _mm256_packus_epi16(md_lo, md_hi);
        packed = _mm256_and_si256(packed, colormask_);
        return _mm256_or_si256(packed, ma);
    }
};

// ----------------------------------------------------------------------------
// ConstColorAlphaBlend_d (destructive、TVPOpacityOnOpacityTable 使用)
// ----------------------------------------------------------------------------

struct avx2_const_alpha_fill_blend_d_functor {
    const tjs_int32 opa32_;
    const __m256i colormask_;
    const __m256i zero_;
    const __m256i opa_;
    __m256i color_;
    inline avx2_const_alpha_fill_blend_d_functor(tjs_int32 opa, tjs_int32 color) :
        opa32_(opa << 8),
        colormask_(_mm256_set1_epi32(0x00ffffff)),
        zero_(_mm256_setzero_si256()),
        opa_(_mm256_set1_epi32(opa << 8))
    {
        __m256i c = _mm256_set1_epi32(color & 0x00ffffff);
        color_ = _mm256_unpacklo_epi8(c, zero_);
    }

    inline tjs_uint32 operator()(tjs_uint32 d) const {
        const __m128i zero128 = _mm_setzero_si128();
        tjs_uint32 addr = opa32_ + (d >> 24);
        tjs_uint32 sopa = TVPOpacityOnOpacityTable[addr];
        __m128i ma = _mm_cvtsi32_si128((int)sopa);
        ma = _mm_shufflelo_epi16(ma, _MM_SHUFFLE(0, 0, 0, 0));
        __m128i md = _mm_cvtsi32_si128((int)d);
        __m128i mc = _mm256_castsi256_si128(color_);
        md = _mm_unpacklo_epi8(md, zero128);
        mc = _mm_sub_epi16(mc, md);
        mc = _mm_mullo_epi16(mc, ma);
        md = _mm_slli_epi16(md, 8);
        mc = _mm_add_epi8(mc, md);
        mc = _mm_srli_epi16(mc, 8);
        mc = _mm_packus_epi16(mc, zero128);
        tjs_uint32 ret = (tjs_uint32)_mm_cvtsi128_si32(mc);
        addr = TVPNegativeMulTable[addr] << 24;
        return (ret & 0x00ffffff) | addr;
    }

    inline __m256i operator()(__m256i md1) const {
        __m256i da    = _mm256_srli_epi32(md1, 24);
        __m256i maddr = _mm256_add_epi32(opa_, da);

        // 8 pixel 分の sopa / dopa を scalar gather
        __m256i ma = _mm256_set_epi32(
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 7)],
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 6)],
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 5)],
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 4)],
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 3)],
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 2)],
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 1)],
            TVPOpacityOnOpacityTable[M256I_U32(maddr, 0)]);

        // lane 内 broadcast: [s0 s1 s2 s3 s0 s1 s2 s3] → per-pixel 4 u16
        ma = _mm256_packs_epi32(ma, ma);
        ma = _mm256_unpacklo_epi16(ma, ma);   // [s0,s0,s1,s1,s2,s2,s3,s3]
        __m256i ma_hi = _mm256_unpackhi_epi16(ma, ma);  // s2,s2,s2,s2,s3,s3,s3,s3
        __m256i ma_lo = _mm256_unpacklo_epi16(ma, ma);  // s0,s0,s0,s0,s1,s1,s1,s1

        __m256i md_lo = _mm256_unpacklo_epi8(md1, zero_);
        __m256i md_hi = _mm256_unpackhi_epi8(md1, zero_);

        __m256i mc_lo = _mm256_sub_epi16(color_, md_lo);
        mc_lo = _mm256_mullo_epi16(mc_lo, ma_lo);
        md_lo = _mm256_slli_epi16(md_lo, 8);
        mc_lo = _mm256_add_epi16(mc_lo, md_lo);
        mc_lo = _mm256_srli_epi16(mc_lo, 8);

        __m256i mc_hi = _mm256_sub_epi16(color_, md_hi);
        mc_hi = _mm256_mullo_epi16(mc_hi, ma_hi);
        md_hi = _mm256_slli_epi16(md_hi, 8);
        mc_hi = _mm256_add_epi16(mc_hi, md_hi);
        mc_hi = _mm256_srli_epi16(mc_hi, 8);

        __m256i rgb = _mm256_and_si256(
            _mm256_packus_epi16(mc_lo, mc_hi), colormask_);

        // dest alpha: TVPNegativeMulTable を直接 gather して 24bit シフト
        tjs_uint32 da0 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 0)] << 24;
        tjs_uint32 da1 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 1)] << 24;
        tjs_uint32 da2 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 2)] << 24;
        tjs_uint32 da3 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 3)] << 24;
        tjs_uint32 da4 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 4)] << 24;
        tjs_uint32 da5 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 5)] << 24;
        tjs_uint32 da6 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 6)] << 24;
        tjs_uint32 da7 = (tjs_uint32)TVPNegativeMulTable[M256I_U32(maddr, 7)] << 24;
        __m256i new_a = _mm256_set_epi32(
            (int)da7, (int)da6, (int)da5, (int)da4,
            (int)da3, (int)da2, (int)da1, (int)da0);
        return _mm256_or_si256(rgb, new_a);
    }
};

// ----------------------------------------------------------------------------
// ConstColorAlphaBlend_a (additive alpha)
// ----------------------------------------------------------------------------

struct avx2_const_alpha_fill_blend_a_functor {
    __m256i mo_;
    __m256i mc_;
    const __m256i zero_;
    inline avx2_const_alpha_fill_blend_a_functor(tjs_int32 opa, tjs_int32 color) :
        zero_(_mm256_setzero_si256())
    {
        opa += opa >> 7;
        mo_ = _mm256_set1_epi16((short)opa);

        // SSE2 と同じ msa 構築 (alpha lane に opa<<24)
        __m128i msa128 = _mm_cvtsi32_si128((int)(opa << 24));
        msa128 = _mm_shuffle_epi32(msa128, _MM_SHUFFLE(0, 0, 0, 0));
        msa128 = _mm_unpacklo_epi8(msa128, _mm_setzero_si128());
        __m256i msa = _mm256_broadcastsi128_si256(msa128);

        __m128i mc128 = _mm_cvtsi32_si128(color & 0x00ffffff);
        mc128 = _mm_shuffle_epi32(mc128, _MM_SHUFFLE(0, 0, 0, 0));
        mc128 = _mm_unpacklo_epi8(mc128, _mm_setzero_si128());
        __m256i mc = _mm256_broadcastsi128_si256(mc128);
        mc = _mm256_mullo_epi16(mc, mo_);
        mc = _mm256_srli_epi16(mc, 8);
        mc_ = _mm256_or_si256(mc, msa);
    }

    inline tjs_uint32 operator()(tjs_uint32 d) const {
        const __m128i zero128 = _mm_setzero_si128();
        __m128i md = _mm_cvtsi32_si128((int)d);
        md = _mm_unpacklo_epi8(md, zero128);
        __m128i md2 = _mm_mullo_epi16(md, _mm256_castsi256_si128(mo_));
        md2 = _mm_srli_epi16(md2, 8);
        md = _mm_sub_epi16(md, md2);
        md = _mm_add_epi16(md, _mm256_castsi256_si128(mc_));
        md = _mm_packus_epi16(md, zero128);
        return (tjs_uint32)_mm_cvtsi128_si32(md);
    }

    inline __m256i operator()(__m256i md1) const {
        __m256i md_lo = _mm256_unpacklo_epi8(md1, zero_);
        __m256i md_hi = _mm256_unpackhi_epi8(md1, zero_);

        __m256i mt_lo = _mm256_srli_epi16(_mm256_mullo_epi16(md_lo, mo_), 8);
        md_lo = _mm256_sub_epi16(md_lo, mt_lo);
        md_lo = _mm256_add_epi16(md_lo, mc_);

        __m256i mt_hi = _mm256_srli_epi16(_mm256_mullo_epi16(md_hi, mo_), 8);
        md_hi = _mm256_sub_epi16(md_hi, mt_hi);
        md_hi = _mm256_add_epi16(md_hi, mc_);

        return _mm256_packus_epi16(md_lo, md_hi);
    }
};

template<typename functor>
static inline void avx2_const_color_alpha_blend(
    tjs_uint32 *dest, tjs_int len, const functor &func)
{
    if (len <= 0) return;
    tjs_int count = (tjs_int)((uintptr_t)dest & 0x1F);
    if (count) {
        count = (32 - count) >> 2;
        if (count > len) count = len;
        tjs_uint32 *limit = dest + count;
        while (dest < limit) {
            *dest = func(*dest);
            ++dest;
        }
        len -= count;
    }
    tjs_int rem       = (len >> 3) << 3;  // 8 pixel ごと
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        __m256i md = _mm256_load_si256((const __m256i *)dest);
        _mm256_store_si256((__m256i *)dest, func(md));
        dest += 8;
    }
    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest);
        ++dest;
    }
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// エクスポート
// ----------------------------------------------------------------------------

void TVPFillARGB_avx2_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 value)
{
    avx2_fill32(dest, len, _mm256_set1_epi32((int)value));
}

void TVPFillARGB_NC_avx2_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 value)
{
    avx2_fill32_nc(dest, len, _mm256_set1_epi32((int)value));
}

void TVPFillColor_avx2_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 color)
{
    avx2_const_color_copy_functor func(color);
    avx2_const_color_copy_unroll(dest, len, func);
}

void TVPFillMask_avx2_c(tjs_uint32 *dest, tjs_int len, tjs_uint32 mask)
{
    avx2_const_alpha_copy_functor func(mask);
    avx2_const_color_copy_unroll(dest, len, func);
}

void TVPConstColorAlphaBlend_avx2_c(
    tjs_uint32 *dest, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    avx2_const_alpha_fill_blend_functor func(opa, (tjs_int32)color);
    avx2_const_color_alpha_blend(dest, len, func);
}

void TVPConstColorAlphaBlend_d_avx2_c(
    tjs_uint32 *dest, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    avx2_const_alpha_fill_blend_d_functor func(opa, (tjs_int32)color);
    avx2_const_color_alpha_blend(dest, len, func);
}

void TVPConstColorAlphaBlend_a_avx2_c(
    tjs_uint32 *dest, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    avx2_const_alpha_fill_blend_a_functor func(opa, (tjs_int32)color);
    avx2_const_color_alpha_blend(dest, len, func);
}
