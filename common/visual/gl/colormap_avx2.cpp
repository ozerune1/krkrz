// AVX2 版 ApplyColorMap ファミリ (Phase 2 colormap)
//
// SSE2 実装 (colormap_sse2.cpp) の 4 pixel/iter を AVX2 で 8 pixel/iter に
// 拡張したもの。アルゴリズムは SSE2 と 1:1 対応:
//
//   md1 (8 pixel = __m256i) を unpacklo/hi_epi8 で 16 u16 × 2 blocks に分割:
//     md_lo = lane 0: pixels 0,1 / lane 1: pixels 4,5  (unpacklo_epi8)
//     md_hi = lane 0: pixels 2,3 / lane 1: pixels 6,7  (unpackhi_epi8)
//
//   AVX2 の 256bit unpack は 128bit lane 内で動くため、ピクセル順序は
//   {0,1,4,5} / {2,3,6,7} にシャッフルされるが、各ピクセルは独立に blend
//   されるので最終的な packus_epi16 で正しい順序に戻る。
//
//   src (8 byte の mask) も同じ layout で展開する: set_epi16 で scalar
//   読み込みから {s5,s5,s5,s5, s4,s4,s4,s4, s1,s1,s1,s1, s0,s0,s0,s0} を
//   構成。ホットパスの演算ではないので scalar gather で十分。
//
// tshift=6 (65) は srai_epi16 + add_epi16、tshift=8 は srli_epi16 + add_epi8
// という 2 種類の丸め trick を SSE2 からそのまま受け継ぐ (SSE2 コメント参照)。
//
// 非 HDA 仕様: 結果 alpha を 0x00ffffff マスクで 0 に。SSE2 修正 (commit
// c2fac7e2) と同じ方針。opaque fast path 用の color_ も alpha を 0 に。
//
// 本コミット C3: base + _o の 4 関数のみ (ColorMap / ColorMap65 /
// ColorMap_o / ColorMap65_o)。_a / _ao / _d は次コミット (C4)。

#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "simd_def_x86x64.h"
#include <string.h>

extern "C" {
extern unsigned char TVPNegativeMulTable65[65 * 256];
extern unsigned char TVPOpacityOnOpacityTable65[65 * 256];
}

namespace {

// ----------------------------------------------------------------------------
// base functor: tshift=6 (65 variant)
// ----------------------------------------------------------------------------
template<int tshift>
struct avx2_apply_color_map_xx_functor {
    __m256i mc_;        // color unpacked to 16 u16 (same in both 128bit lanes)
    __m256i color_;     // opaque fast path 用 (alpha=0 にマスク済み)
    const __m256i zero_;
    const __m256i rgb_mask_;

    inline avx2_apply_color_map_xx_functor(tjs_uint32 color) :
        zero_(_mm256_setzero_si256()),
        rgb_mask_(_mm256_set1_epi32(0x00ffffff))
    {
        __m256i c = _mm256_set1_epi32((int)color);
        // 非 HDA 仕様: opaque fast path 用の color_ は alpha=0
        color_ = _mm256_and_si256(c, rgb_mask_);
        // mc_ は 8 u16 × 2 lanes。各 lane 内で
        //   (0A 0R 0G 0B 0A 0R 0G 0B) の 8 u16 (2 pixel 分の broadcast)
        mc_ = _mm256_unpacklo_epi8(c, zero_);
    }

    // スカラ 1 pixel 版 (SSE2 intrinsics を 128bit で使う)
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        const __m128i zero128 = _mm_setzero_si128();
        __m128i md = _mm_cvtsi32_si128((int)d);
        md = _mm_unpacklo_epi8(md, zero128);
        __m128i mo = _mm_cvtsi32_si128((int)(tjs_uint32)s);
        mo = _mm_shufflelo_epi16(mo, _MM_SHUFFLE(0, 0, 0, 0));
        __m128i mc = _mm256_castsi256_si128(mc_);
        mc = _mm_sub_epi16(mc, md);
        mc = _mm_mullo_epi16(mc, mo);
        mc = _mm_srai_epi16(mc, tshift);
        md = _mm_add_epi16(md, mc);
        md = _mm_packus_epi16(md, zero128);
        return (tjs_uint32)_mm_cvtsi128_si32(md) & 0x00ffffffu;
    }

    // ベクタ 8 pixel 版
    inline __m256i operator()(__m256i md1, __m256i mo_lo, __m256i mo_hi) const {
        __m256i md_lo = _mm256_unpacklo_epi8(md1, zero_);  // pixels 0,1 | 4,5
        __m256i md_hi = _mm256_unpackhi_epi8(md1, zero_);  // pixels 2,3 | 6,7

        // lo half
        __m256i mc_lo = _mm256_sub_epi16(mc_, md_lo);   // c - d
        mc_lo = _mm256_mullo_epi16(mc_lo, mo_lo);       // *= opa
        mc_lo = _mm256_srai_epi16(mc_lo, tshift);        // >>= tshift (signed)
        md_lo = _mm256_add_epi16(md_lo, mc_lo);          // d += c

        // hi half
        __m256i mc_hi = _mm256_sub_epi16(mc_, md_hi);
        mc_hi = _mm256_mullo_epi16(mc_hi, mo_hi);
        mc_hi = _mm256_srai_epi16(mc_hi, tshift);
        md_hi = _mm256_add_epi16(md_hi, mc_hi);

        // pack + 非 HDA alpha=0 mask
        return _mm256_and_si256(_mm256_packus_epi16(md_lo, md_hi), rgb_mask_);
    }
};

// ----------------------------------------------------------------------------
// base functor: tshift=8 (256 variant)
//
// SSE2 版と同じ srli_epi16 + add_epi8 の byte-level wrap trick を使う。
// mullo_epi16 で signed 意味を保持しつつ、srli で上位 8bit を取り出し、
// add_epi8 で byte 単位 wrap して最終値を得る。詳細は SSE2 の該当
// functor コメント参照。
// ----------------------------------------------------------------------------
struct avx2_apply_color_map256_functor {
    __m256i mc_;
    __m256i color_;
    const __m256i zero_;
    const __m256i rgb_mask_;

    inline avx2_apply_color_map256_functor(tjs_uint32 color) :
        zero_(_mm256_setzero_si256()),
        rgb_mask_(_mm256_set1_epi32(0x00ffffff))
    {
        __m256i c = _mm256_set1_epi32((int)color);
        color_ = _mm256_and_si256(c, rgb_mask_);
        mc_ = _mm256_unpacklo_epi8(c, zero_);
    }

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        const __m128i zero128 = _mm_setzero_si128();
        __m128i md = _mm_cvtsi32_si128((int)d);
        md = _mm_unpacklo_epi8(md, zero128);
        __m128i mo = _mm_cvtsi32_si128((int)(tjs_uint32)s);
        mo = _mm_shufflelo_epi16(mo, _MM_SHUFFLE(0, 0, 0, 0));
        __m128i mc = _mm256_castsi256_si128(mc_);
        mc = _mm_sub_epi16(mc, md);
        mc = _mm_mullo_epi16(mc, mo);
        mc = _mm_srli_epi16(mc, 8);
        md = _mm_add_epi8(md, mc);
        md = _mm_packus_epi16(md, zero128);
        return (tjs_uint32)_mm_cvtsi128_si32(md) & 0x00ffffffu;
    }

    inline __m256i operator()(__m256i md1, __m256i mo_lo, __m256i mo_hi) const {
        __m256i md_lo = _mm256_unpacklo_epi8(md1, zero_);
        __m256i md_hi = _mm256_unpackhi_epi8(md1, zero_);

        __m256i mc_lo = _mm256_sub_epi16(mc_, md_lo);
        mc_lo = _mm256_mullo_epi16(mc_lo, mo_lo);
        mc_lo = _mm256_srli_epi16(mc_lo, 8);
        md_lo = _mm256_add_epi8(md_lo, mc_lo);

        __m256i mc_hi = _mm256_sub_epi16(mc_, md_hi);
        mc_hi = _mm256_mullo_epi16(mc_hi, mo_hi);
        mc_hi = _mm256_srli_epi16(mc_hi, 8);
        md_hi = _mm256_add_epi8(md_hi, mc_hi);

        return _mm256_and_si256(_mm256_packus_epi16(md_lo, md_hi), rgb_mask_);
    }
};

// ----------------------------------------------------------------------------
// wrapper: straight / _o
// ----------------------------------------------------------------------------
template<typename tbase>
struct avx2_apply_color_map_xx_straight_functor : tbase {
    inline avx2_apply_color_map_xx_straight_functor(tjs_uint32 color) : tbase(color) {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        return tbase::operator()(d, s);
    }
    // 8 src bytes → 2 × __m256i (mo_lo / mo_hi) を set_epi16 で構成
    inline __m256i operator()(__m256i md1, const tjs_uint8 *src) const {
        tjs_int16 s0 = src[0], s1 = src[1], s2 = src[2], s3 = src[3];
        tjs_int16 s4 = src[4], s5 = src[5], s6 = src[6], s7 = src[7];
        __m256i mo_lo = _mm256_set_epi16(
            s5, s5, s5, s5, s4, s4, s4, s4,
            s1, s1, s1, s1, s0, s0, s0, s0);
        __m256i mo_hi = _mm256_set_epi16(
            s7, s7, s7, s7, s6, s6, s6, s6,
            s3, s3, s3, s3, s2, s2, s2, s2);
        return tbase::operator()(md1, mo_lo, mo_hi);
    }
};

template<typename tbase>
struct avx2_apply_color_map_xx_o_functor : tbase {
    tjs_int opa32_;
    inline avx2_apply_color_map_xx_o_functor(tjs_uint32 color, tjs_int opa)
        : tbase(color), opa32_(opa) {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        return tbase::operator()(d, (tjs_uint8)((s * opa32_) >> 8));
    }
    // 各 src byte に opa を乗じてから展開
    inline __m256i operator()(__m256i md1, const tjs_uint8 *src) const {
        tjs_int16 s0 = (tjs_uint8)((src[0] * opa32_) >> 8);
        tjs_int16 s1 = (tjs_uint8)((src[1] * opa32_) >> 8);
        tjs_int16 s2 = (tjs_uint8)((src[2] * opa32_) >> 8);
        tjs_int16 s3 = (tjs_uint8)((src[3] * opa32_) >> 8);
        tjs_int16 s4 = (tjs_uint8)((src[4] * opa32_) >> 8);
        tjs_int16 s5 = (tjs_uint8)((src[5] * opa32_) >> 8);
        tjs_int16 s6 = (tjs_uint8)((src[6] * opa32_) >> 8);
        tjs_int16 s7 = (tjs_uint8)((src[7] * opa32_) >> 8);
        __m256i mo_lo = _mm256_set_epi16(
            s5, s5, s5, s5, s4, s4, s4, s4,
            s1, s1, s1, s1, s0, s0, s0, s0);
        __m256i mo_hi = _mm256_set_epi16(
            s7, s7, s7, s7, s6, s6, s6, s6,
            s3, s3, s3, s3, s2, s2, s2, s2);
        return tbase::operator()(md1, mo_lo, mo_hi);
    }
};

typedef avx2_apply_color_map_xx_straight_functor< avx2_apply_color_map_xx_functor<6> >
        avx2_apply_color_map65_functor;
typedef avx2_apply_color_map_xx_straight_functor< avx2_apply_color_map256_functor >
        avx2_apply_color_map_functor;
typedef avx2_apply_color_map_xx_o_functor< avx2_apply_color_map_xx_functor<6> >
        avx2_apply_color_map65_o_functor;
typedef avx2_apply_color_map_xx_o_functor< avx2_apply_color_map256_functor >
        avx2_apply_color_map_o_functor;

// ----------------------------------------------------------------------------
// _a functor (premul alpha 付き)
//
// mc_ レイアウトは SSE2 と同じ: (0B, 0G, 0R, 0x0100) × 2 の 8 u16 を 128bit
// 側で構築し、broadcasti128 で 256bit 両 lane に複製する。0x0100 が alpha
// 位置に入るのは `Sa * 0x100 >> tshift` = Sa (tshift=8) または Sa*4
// (tshift=6) を alpha の出力ソースとして使うため。
// ----------------------------------------------------------------------------
template<int tshift>
struct avx2_apply_color_map_xx_a_functor {
    __m256i mc_;
    __m256i color_;
    const __m256i zero_;

    inline avx2_apply_color_map_xx_a_functor(tjs_uint32 color) :
        zero_(_mm256_setzero_si256())
    {
        // 128bit で SSE2 と同じ mc_ を構築
        const __m128i zero128 = _mm_setzero_si128();
        __m128i mc128 = _mm_cvtsi32_si128((int)(color & 0x00ffffff));
        __m128i tmp = _mm_cvtsi32_si128(0x100);
        tmp = _mm_slli_epi64(tmp, 48);   // = 0x0100_0000_0000_0000
        mc128 = _mm_unpacklo_epi8(mc128, zero128);
        mc128 = _mm_or_si128(mc128, tmp);
        mc128 = _mm_shuffle_epi32(mc128, _MM_SHUFFLE(1, 0, 1, 0));
        // 両 lane に broadcast
        mc_ = _mm256_broadcastsi128_si256(mc128);

        // color_ は apply_color_map_func_avx2 (非 branch) から参照されないが
        // 一応 alpha=0 で埋めておく
        color_ = _mm256_and_si256(
            _mm256_set1_epi32((int)color),
            _mm256_set1_epi32(0x00ffffff));
    }

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        const __m128i zero128 = _mm_setzero_si128();
        __m128i mo = _mm_cvtsi32_si128((int)(tjs_uint32)s);
        mo = _mm_shufflelo_epi16(mo, _MM_SHUFFLE(0, 0, 0, 0));
        __m128i mc128 = _mm256_castsi256_si128(mc_);
        __m128i ms = mo;
        ms = _mm_mullo_epi16(ms, mc128);   // alpha * color
        ms = _mm_srli_epi16(ms, tshift);
        __m128i md = _mm_cvtsi32_si128((int)d);
        md = _mm_unpacklo_epi8(md, zero128);
        __m128i mds = md;
        mds = _mm_mullo_epi16(mds, mo);
        mds = _mm_srli_epi16(mds, tshift);
        md = _mm_sub_epi16(md, mds);        // d - d*s
        md = _mm_add_epi16(md, ms);          // + s*c
        md = _mm_packus_epi16(md, zero128);
        return (tjs_uint32)_mm_cvtsi128_si32(md);
    }

    inline __m256i operator()(__m256i md1, __m256i mo_lo, __m256i mo_hi) const {
        __m256i md_lo = _mm256_unpacklo_epi8(md1, zero_);
        __m256i md_hi = _mm256_unpackhi_epi8(md1, zero_);

        // lo half: ms = mo * mc >> shift, mds = md * mo >> shift, md = md - mds + ms
        __m256i ms_lo  = _mm256_srli_epi16(_mm256_mullo_epi16(mo_lo, mc_), tshift);
        __m256i mds_lo = _mm256_srli_epi16(_mm256_mullo_epi16(md_lo, mo_lo), tshift);
        md_lo = _mm256_add_epi16(_mm256_sub_epi16(md_lo, mds_lo), ms_lo);

        __m256i ms_hi  = _mm256_srli_epi16(_mm256_mullo_epi16(mo_hi, mc_), tshift);
        __m256i mds_hi = _mm256_srli_epi16(_mm256_mullo_epi16(md_hi, mo_hi), tshift);
        md_hi = _mm256_add_epi16(_mm256_sub_epi16(md_hi, mds_hi), ms_hi);

        return _mm256_packus_epi16(md_lo, md_hi);
    }
};

typedef avx2_apply_color_map_xx_straight_functor< avx2_apply_color_map_xx_a_functor<6> >
        avx2_apply_color_map65_a_functor;
typedef avx2_apply_color_map_xx_straight_functor< avx2_apply_color_map_xx_a_functor<8> >
        avx2_apply_color_map_a_functor;
typedef avx2_apply_color_map_xx_o_functor< avx2_apply_color_map_xx_a_functor<6> >
        avx2_apply_color_map65_ao_functor;
typedef avx2_apply_color_map_xx_o_functor< avx2_apply_color_map_xx_a_functor<8> >
        avx2_apply_color_map_ao_functor;

// ----------------------------------------------------------------------------
// _d functor (destructive, 65 only)
//
// TVPNegativeMulTable65 で dest alpha 更新、TVPOpacityOnOpacityTable65 で
// RGB ブレンド係数を決定する。per-pixel の (s, d.alpha) 組でテーブル参照
// するので、base/_a と違って mo の broadcast ではなく scalar gather を使う。
// ----------------------------------------------------------------------------
struct avx2_apply_color_map65_d_functor {
    __m256i mc_;
    __m256i color_;
    const __m256i zero_;
    const __m256i rgb_mask_;

    inline avx2_apply_color_map65_d_functor(tjs_uint32 color) :
        zero_(_mm256_setzero_si256()),
        rgb_mask_(_mm256_set1_epi32(0x00ffffff))
    {
        __m256i c = _mm256_set1_epi32((int)(color | 0xff000000));
        color_ = c;  // _d では opaque fast path は使わない (_a と同じく func_avx2 経路)
        mc_ = _mm256_unpacklo_epi8(c, zero_);
    }

    // スカラ版 (SSE2 _d と 1:1 対応)
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint8 s) const {
        if (s == 0) return d;
        const __m128i zero128 = _mm_setzero_si128();
        __m128i mc = _mm256_castsi256_si128(mc_);
        __m128i md = _mm_cvtsi32_si128((int)d);
        md = _mm_unpacklo_epi8(md, zero128);
        tjs_int  addr = (s << 8) | (d >> 24);
        tjs_uint32 dopa = TVPNegativeMulTable65[addr];
        tjs_uint32 o    = TVPOpacityOnOpacityTable65[addr];
        dopa <<= 24;
        __m128i mo = _mm_cvtsi32_si128((int)o);
        mo = _mm_shufflelo_epi16(mo, _MM_SHUFFLE(0, 0, 0, 0));
        mc = _mm_sub_epi16(mc, md);
        mc = _mm_mullo_epi16(mc, mo);
        md = _mm_slli_epi16(md, 8);
        md = _mm_add_epi16(md, mc);
        md = _mm_srli_epi16(md, 8);
        md = _mm_packus_epi16(md, zero128);
        tjs_uint32 ret = (tjs_uint32)_mm_cvtsi128_si32(md);
        return (ret & 0x00ffffffu) | dopa;
    }

    // ベクタ版: 8 pixel 分の (s, d.alpha) から scalar gather で
    // TVPOpacityOnOpacityTable65 / TVPNegativeMulTable65 を引いて、
    // その係数で RGB を blend + alpha を上書きする。
    inline __m256i operator()(__m256i md1, const tjs_uint8 *src) const {
        // 8 pixel の d.alpha を抽出
        alignas(32) tjs_uint32 d_arr[8];
        _mm256_store_si256((__m256i *)d_arr, md1);

        // addr[i] = (src[i]<<8) | (d_arr[i]>>24) で 8 pixel 分
        tjs_uint32 addr0 = ((tjs_uint32)src[0] << 8) | (d_arr[0] >> 24);
        tjs_uint32 addr1 = ((tjs_uint32)src[1] << 8) | (d_arr[1] >> 24);
        tjs_uint32 addr2 = ((tjs_uint32)src[2] << 8) | (d_arr[2] >> 24);
        tjs_uint32 addr3 = ((tjs_uint32)src[3] << 8) | (d_arr[3] >> 24);
        tjs_uint32 addr4 = ((tjs_uint32)src[4] << 8) | (d_arr[4] >> 24);
        tjs_uint32 addr5 = ((tjs_uint32)src[5] << 8) | (d_arr[5] >> 24);
        tjs_uint32 addr6 = ((tjs_uint32)src[6] << 8) | (d_arr[6] >> 24);
        tjs_uint32 addr7 = ((tjs_uint32)src[7] << 8) | (d_arr[7] >> 24);

        // 8 pixel 分の OpacityOnOpacityTable65 を取ってブレンド係数 mo を構築
        tjs_int16 o0 = TVPOpacityOnOpacityTable65[addr0];
        tjs_int16 o1 = TVPOpacityOnOpacityTable65[addr1];
        tjs_int16 o2 = TVPOpacityOnOpacityTable65[addr2];
        tjs_int16 o3 = TVPOpacityOnOpacityTable65[addr3];
        tjs_int16 o4 = TVPOpacityOnOpacityTable65[addr4];
        tjs_int16 o5 = TVPOpacityOnOpacityTable65[addr5];
        tjs_int16 o6 = TVPOpacityOnOpacityTable65[addr6];
        tjs_int16 o7 = TVPOpacityOnOpacityTable65[addr7];
        __m256i mo_lo = _mm256_set_epi16(
            o5, o5, o5, o5, o4, o4, o4, o4,
            o1, o1, o1, o1, o0, o0, o0, o0);
        __m256i mo_hi = _mm256_set_epi16(
            o7, o7, o7, o7, o6, o6, o6, o6,
            o3, o3, o3, o3, o2, o2, o2, o2);

        // 同じ layout で展開した md
        __m256i md_lo = _mm256_unpacklo_epi8(md1, zero_);
        __m256i md_hi = _mm256_unpackhi_epi8(md1, zero_);

        // md = (md << 8 + (mc - md) * mo) >> 8  (SSE2 と同じ計算式)
        __m256i mc_lo = _mm256_sub_epi16(mc_, md_lo);
        mc_lo = _mm256_mullo_epi16(mc_lo, mo_lo);
        md_lo = _mm256_slli_epi16(md_lo, 8);
        md_lo = _mm256_add_epi16(md_lo, mc_lo);
        md_lo = _mm256_srli_epi16(md_lo, 8);

        __m256i mc_hi = _mm256_sub_epi16(mc_, md_hi);
        mc_hi = _mm256_mullo_epi16(mc_hi, mo_hi);
        md_hi = _mm256_slli_epi16(md_hi, 8);
        md_hi = _mm256_add_epi16(md_hi, mc_hi);
        md_hi = _mm256_srli_epi16(md_hi, 8);

        __m256i rgb = _mm256_and_si256(
            _mm256_packus_epi16(md_lo, md_hi), rgb_mask_);

        // dest alpha は TVPNegativeMulTable65 から直接引いて 24bit シフト
        tjs_uint32 da0 = (tjs_uint32)TVPNegativeMulTable65[addr0] << 24;
        tjs_uint32 da1 = (tjs_uint32)TVPNegativeMulTable65[addr1] << 24;
        tjs_uint32 da2 = (tjs_uint32)TVPNegativeMulTable65[addr2] << 24;
        tjs_uint32 da3 = (tjs_uint32)TVPNegativeMulTable65[addr3] << 24;
        tjs_uint32 da4 = (tjs_uint32)TVPNegativeMulTable65[addr4] << 24;
        tjs_uint32 da5 = (tjs_uint32)TVPNegativeMulTable65[addr5] << 24;
        tjs_uint32 da6 = (tjs_uint32)TVPNegativeMulTable65[addr6] << 24;
        tjs_uint32 da7 = (tjs_uint32)TVPNegativeMulTable65[addr7] << 24;
        __m256i da = _mm256_set_epi32(
            (int)da7, (int)da6, (int)da5, (int)da4,
            (int)da3, (int)da2, (int)da1, (int)da0);

        return _mm256_or_si256(rgb, da);
    }
};

// ----------------------------------------------------------------------------
// branch / plain loops (8 pixel/iter)
// ----------------------------------------------------------------------------
template<typename functor, tjs_uint64 topaque64>
static inline void apply_color_map_branch_func_avx2(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, const functor &func)
{
    if (len <= 0) return;

    // 32 byte アライメント調整
    tjs_int count = (tjs_int)((uintptr_t)dest & 0x1F);
    if (count) {
        count = (32 - count) >> 2;
        if (count > len) count = len;
        tjs_uint32 *limit = dest + count;
        while (dest < limit) {
            *dest = func(*dest, *src);
            ++dest; ++src;
        }
        len -= count;
    }
    tjs_int rem = (len >> 3) << 3;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        tjs_uint64 s8;
        memcpy(&s8, src, sizeof(s8));
        if (s8 == topaque64) {
            _mm256_store_si256((__m256i *)dest, func.color_);
        } else if (s8 != 0) {
            __m256i md = _mm256_load_si256((const __m256i *)dest);
            _mm256_store_si256((__m256i *)dest, func(md, src));
        } // else: 完全透明、何もしない
        dest += 8; src += 8;
    }
    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest, *src);
        ++dest; ++src;
    }
}

template<typename functor>
static inline void apply_color_map_func_avx2(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, const functor &func)
{
    if (len <= 0) return;

    tjs_int count = (tjs_int)((uintptr_t)dest & 0x1F);
    if (count) {
        count = (32 - count) >> 2;
        if (count > len) count = len;
        tjs_uint32 *limit = dest + count;
        while (dest < limit) {
            *dest = func(*dest, *src);
            ++dest; ++src;
        }
        len -= count;
    }
    tjs_int rem = (len >> 3) << 3;
    tjs_uint32 *limit = dest + rem;
    while (dest < limit) {
        __m256i md = _mm256_load_si256((const __m256i *)dest);
        _mm256_store_si256((__m256i *)dest, func(md, src));
        dest += 8; src += 8;
    }
    limit += (len - rem);
    while (dest < limit) {
        *dest = func(*dest, *src);
        ++dest; ++src;
    }
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// エクスポート: 4 関数 (base + _o)
// ----------------------------------------------------------------------------

// 65 は src byte が 0..64 (topaque=0x40) 前提
static const tjs_uint64 CM65_TOPAQUE = 0x4040404040404040ULL;
static const tjs_uint64 CM256_TOPAQUE = 0xffffffffffffffffULL;

void TVPApplyColorMap65_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    avx2_apply_color_map65_functor func(color);
    apply_color_map_branch_func_avx2<avx2_apply_color_map65_functor, CM65_TOPAQUE>(
        dest, src, len, func);
}

void TVPApplyColorMap_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    avx2_apply_color_map_functor func(color);
    apply_color_map_branch_func_avx2<avx2_apply_color_map_functor, CM256_TOPAQUE>(
        dest, src, len, func);
}

void TVPApplyColorMap65_o_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    avx2_apply_color_map65_o_functor func(color, opa);
    apply_color_map_func_avx2(dest, src, len, func);
}

void TVPApplyColorMap_o_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    avx2_apply_color_map_o_functor func(color, opa);
    apply_color_map_func_avx2(dest, src, len, func);
}

// C4: _a / _ao / _d
void TVPApplyColorMap65_a_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    avx2_apply_color_map65_a_functor func(color);
    apply_color_map_func_avx2(dest, src, len, func);
}

void TVPApplyColorMap_a_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    avx2_apply_color_map_a_functor func(color);
    apply_color_map_func_avx2(dest, src, len, func);
}

void TVPApplyColorMap65_ao_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    avx2_apply_color_map65_ao_functor func(color, opa);
    apply_color_map_func_avx2(dest, src, len, func);
}

void TVPApplyColorMap_ao_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color, tjs_int opa)
{
    avx2_apply_color_map_ao_functor func(color, opa);
    apply_color_map_func_avx2(dest, src, len, func);
}

void TVPApplyColorMap65_d_avx2_c(
    tjs_uint32 *dest, const tjs_uint8 *src, tjs_int len, tjs_uint32 color)
{
    avx2_apply_color_map65_d_functor func(color);
    apply_color_map_func_avx2(dest, src, len, func);
}
