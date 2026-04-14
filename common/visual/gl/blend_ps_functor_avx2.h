
#ifndef __BLEND_PS_FUNCTOR_AVX2_H__
#define __BLEND_PS_FUNCTOR_AVX2_H__

#include "blend_functor_c.h"

// PsBlend (Photoshop 互換ブレンド) の AVX2 実装。
// SSE2 ファンクタ (blend_ps_functor_sse2.h) を 256bit / 8 pixel 単位に
// 拡張したもので、計算式 (>>25 + srai_epi16(_, 7) で a を 7bit 量子化) も
// SSE2 と同一にしてある。Photoshop 互換ブレンドの SIMD 最適化として広く
// 許容される範囲の精度差で、SIMD parity test では PsBlend ファミリ専用の
// トレランス (tol_rgb=2 / ColorDodge5 だけ tol_rgb=8、非 HDA は alpha 無視)
// を適用して通している。詳細は memory `project_psblend_tolerance.md` 参照。
//
// scalar overload (tjs_uint32, tjs_uint32, tjs_uint32) は per-pixel 処理で
// 256bit を使う旨味が無いため SSE2 と同じく 128bit 版を呼ぶ実装にしている。

template<typename blend_func>
struct avx2_ps_variation : public blend_func {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 a = (s>>25);	// 7bit 量子化 (SSE2 と同じ精度)
		return blend_func::operator()( d, s, a );
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		__m256i a = _mm256_srli_epi32( s, 25 );
		return blend_func::operator()( d, s, a );
	}
};
template<typename blend_func>
struct avx2_ps_variation_opa : public blend_func {
	const tjs_int32 opa_;
	const __m256i opa256_;
	avx2_ps_variation_opa( tjs_int32 opa ) : opa_(opa), opa256_(_mm256_set1_epi32(opa)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 a = ((s>>25) * opa_) >> 8;
		return blend_func::operator()( d, s, a );
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		__m256i a = _mm256_srli_epi32( s, 25 );
		a = _mm256_mullo_epi16( a, opa256_ );
		a = _mm256_srli_epi32( a, 8 );
		return blend_func::operator()( d, s, a );
	}
};

//-------------------------------------
// pack_alpha: 各 pixel の a を 4x16bit slot に broadcast する。
// AVX2 unpack は 128bit lane 内で動くので、ベクトル版は 1 つの __m256i を
// 「下位 128bit に lane 0,1, 上位 128bit に lane 2,3 を持つ」という形に
// 整形する pack_alpha1 + pack_alpha12 + pack_alpha2 の 3 段構成 (SSE2 と
// 同じパターン)。
struct avx2_ps_pack_func {
	// scalar 版 (__m128i 1pixel)
	static inline __m128i pack_alpha( __m128i ma ) {
		ma = _mm_unpacklo_epi16( ma, ma );		// = 0000000000AA00AA
		return _mm_unpacklo_epi32( ma, ma );	// = 00AA00AA00AA00AA
	}
	// 4pixel 版 (__m256i)
	static inline __m256i pack_alpha1( __m256i ma ) {
		ma = _mm256_packs_epi32( ma, ma );			// 0 1 2 3 0 1 2 3 (per 128bit lane)
		return _mm256_unpacklo_epi16( ma, ma );		// 0 0 1 1 2 2 3 3
	}
	static inline __m256i pack_alpha12( __m256i ma ) {
		return _mm256_unpacklo_epi16( ma, ma );		// 0 0 0 0 1 1 1 1
	}
	static inline __m256i pack_alpha2( __m256i ma ) {
		return _mm256_unpackhi_epi16( ma, ma );		// 2 2 2 2 3 3 3 3
	}
};
struct avx2_ps_pack_hda_func {
	// HDA: alpha lane の broadcast slot を 0 にすることで result.alpha = dst.alpha
	// となる ((s-d)*0/128 + d.alpha)
	static inline __m128i pack_alpha( __m128i ma ) {
		__m128i ma2 = ma;
		ma = _mm_unpacklo_epi16( ma, ma );
		return _mm_unpacklo_epi32( ma, ma2 );	// 000000AA00AA00AA (alpha lane = 0)
	}
	static inline __m256i pack_alpha1( __m256i ma ) {
		ma = _mm256_packs_epi32( ma, ma );
		return _mm256_unpacklo_epi16( ma, ma );
	}
	static inline __m256i pack_alpha12( __m256i ma ) {
		ma = _mm256_unpacklo_epi16( ma, ma );
		return _mm256_srli_epi64( ma, 16 );		// alpha lane を 0 へ落とす
	}
	static inline __m256i pack_alpha2( __m256i ma ) {
		ma = _mm256_unpackhi_epi16( ma, ma );
		return _mm256_srli_epi64( ma, 16 );
	}
};

//-------------------------------------
struct avx2_ps_nullblend_func {
	inline void blend( __m128i& md, __m128i& ms ) const {}
	inline void blend( __m256i& md, __m256i& ms ) const {}
};

//-------------------------------------
template<typename pack_func, typename blend_func>
struct avx2_ps_alpha_blend {
	const __m128i zero128_;
	const __m256i zero_;
	const blend_func blend_;
	inline avx2_ps_alpha_blend()
		: zero128_( _mm_setzero_si128() ), zero_( _mm256_setzero_si256() ), blend_() {}
	// scalar (1 pixel) — SSE2 と同じ 128bit 演算
	inline tjs_uint32 one( __m128i md, __m128i ms, __m128i ma ) const {
		ms = _mm_unpacklo_epi8( ms, zero128_ );	// 00AA00RR00GG00BB
		md = _mm_unpacklo_epi8( md, zero128_ );
		blend_.blend( md, ms );
		ma = pack_func::pack_alpha( ma );
		ms = _mm_sub_epi16( ms, md );
		ms = _mm_mullo_epi16( ms, ma );
		ms = _mm_srai_epi16( ms, 7 );
		md = _mm_add_epi16( md, ms );
		md = _mm_packus_epi16( md, zero128_ );
		return _mm_cvtsi128_si32( md );
	}
	// vector (8 pixel)
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		__m256i ms2 = ms1;
		__m256i md2 = md1;

		ms1 = _mm256_unpacklo_epi8( ms1, zero_ );	// 00AA00RR00GG00BB (src l)
		md1 = _mm256_unpacklo_epi8( md1, zero_ );	// 00AA00RR00GG00BB (dst l)
		blend_.blend( md1, ms1 );
		ma1 = pack_func::pack_alpha1( ma1 );
		__m256i ma2 = ma1;
		ma1 = pack_func::pack_alpha12( ma1 );

		ms1 = _mm256_sub_epi16( ms1, md1 );
		ms1 = _mm256_mullo_epi16( ms1, ma1 );
		ms1 = _mm256_srai_epi16( ms1, 7 );
		md1 = _mm256_add_epi16( md1, ms1 );

		ms2 = _mm256_unpackhi_epi8( ms2, zero_ );	// 00AA00RR00GG00BB (src h)
		md2 = _mm256_unpackhi_epi8( md2, zero_ );
		blend_.blend( md2, ms2 );
		ma2 = pack_func::pack_alpha2( ma2 );

		ms2 = _mm256_sub_epi16( ms2, md2 );
		ms2 = _mm256_mullo_epi16( ms2, ma2 );
		ms2 = _mm256_srai_epi16( ms2, 7 );
		md2 = _mm256_add_epi16( md2, ms2 );
		return _mm256_packus_epi16( md1, md2 );
	}
};
typedef avx2_ps_alpha_blend<avx2_ps_pack_func,    avx2_ps_nullblend_func>	avx2_ps_alpha_blend_func;
typedef avx2_ps_alpha_blend<avx2_ps_pack_hda_func, avx2_ps_nullblend_func>	avx2_ps_alpha_blend_hda_func;

//-------------------------------------
// alpha_blend_f: scalar (uint32, uint32, uint32) → __m128i one() を呼ぶ wrapper
template<typename alpha_blend_base>
struct avx2_ps_alpha_blend_f : public alpha_blend_base {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		return alpha_blend_base::one( md, ms, ma );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		return alpha_blend_base::operator()( md1, ms1, ma1 );
	}
};

//-------------------------------------
// PsAlphaBlend (= 純粋アルファブレンド、pre-blend なし)
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend_func>		avx2_ps_alpha_blend_n;
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend_hda_func>	avx2_ps_alpha_blend_hda;

typedef avx2_ps_variation    <avx2_ps_alpha_blend_n>	avx2_ps_alpha_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_alpha_blend_n>	avx2_ps_alpha_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_alpha_blend_hda>	avx2_ps_alpha_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_alpha_blend_hda>	avx2_ps_alpha_blend_hda_o_functor;

//-------------------------------------
// PsAddBlend (linear dodge): pre-blend で ms = sat_add(ms, md)
template<typename alpha_blend_base>
struct avx2_ps_add_blend_func : public alpha_blend_base {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		ms = _mm_adds_epu8( ms, md );
		return alpha_blend_base::one( md, ms, ma );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		ms1 = _mm256_adds_epu8( ms1, md1 );
		return alpha_blend_base::operator()( md1, ms1, ma1 );
	}
};
typedef avx2_ps_add_blend_func<avx2_ps_alpha_blend_func>	avx2_ps_add_blend_n;
typedef avx2_ps_add_blend_func<avx2_ps_alpha_blend_hda_func>	avx2_ps_add_blend_hda;

typedef avx2_ps_variation    <avx2_ps_add_blend_n>		avx2_ps_add_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_add_blend_n>		avx2_ps_add_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_add_blend_hda>	avx2_ps_add_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_add_blend_hda>	avx2_ps_add_blend_hda_o_functor;

//-------------------------------------
// PsSubBlend (linear burn): pre-blend で ms = sat_sub(md, ~ms) = max(0, md+ms-255)
template<typename alpha_blend_base>
struct avx2_ps_sub_blend_func : public alpha_blend_base {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i mall1 = _mm_cmpeq_epi32( ms, ms );	// = 0xff..ff
		ms = _mm_xor_si128( ms, mall1 );			// ~src
		__m128i tmp = _mm_subs_epu8( md, ms );		// sat(md - ~src) = max(0, md+s-255)
		return alpha_blend_base::one( md, tmp, ma );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		__m256i mall1 = _mm256_cmpeq_epi32( ms1, ms1 );
		ms1 = _mm256_xor_si256( ms1, mall1 );
		__m256i tmp = _mm256_subs_epu8( md1, ms1 );
		return alpha_blend_base::operator()( md1, tmp, ma1 );
	}
};
typedef avx2_ps_sub_blend_func<avx2_ps_alpha_blend_func>	avx2_ps_sub_blend_n;
typedef avx2_ps_sub_blend_func<avx2_ps_alpha_blend_hda_func>	avx2_ps_sub_blend_hda;

typedef avx2_ps_variation    <avx2_ps_sub_blend_n>		avx2_ps_sub_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_sub_blend_n>		avx2_ps_sub_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_sub_blend_hda>	avx2_ps_sub_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_sub_blend_hda>	avx2_ps_sub_blend_hda_o_functor;

//-------------------------------------
// PsMulBlend: pre-blend hook で ms = (ms*md)>>8
struct avx2_ps_mul_blend_func {
	inline void blend( __m128i& md, __m128i& ms ) const {
		ms = _mm_mullo_epi16( ms, md );
		ms = _mm_srli_epi16( ms, 8 );
	}
	inline void blend( __m256i& md, __m256i& ms ) const {
		ms = _mm256_mullo_epi16( ms, md );
		ms = _mm256_srli_epi16( ms, 8 );
	}
};
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_func,    avx2_ps_mul_blend_func> >	avx2_ps_mul_blend_n;
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_hda_func, avx2_ps_mul_blend_func> >	avx2_ps_mul_blend_hda;

typedef avx2_ps_variation    <avx2_ps_mul_blend_n>		avx2_ps_mul_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_mul_blend_n>		avx2_ps_mul_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_mul_blend_hda>	avx2_ps_mul_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_mul_blend_hda>	avx2_ps_mul_blend_hda_o_functor;

//-------------------------------------
// PsLightenBlend: pre-blend hook で ms = max(ms, md)
// 16bit unpacked lane でも `subs_epu8` / `add_epi8` の byte 演算が成立する
// (high byte は 0 のまま)。
struct avx2_ps_lighten_blend_func {
	inline void blend( __m128i& md, __m128i& ms ) const {
		__m128i md2 = md;
		md2 = _mm_subs_epu8( md2, ms );
		ms = _mm_add_epi8( ms, md2 );
	}
	inline void blend( __m256i& md, __m256i& ms ) const {
		__m256i md2 = md;
		md2 = _mm256_subs_epu8( md2, ms );
		ms = _mm256_add_epi8( ms, md2 );
	}
};
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_func,    avx2_ps_lighten_blend_func> >	avx2_ps_lighten_blend_n;
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_hda_func, avx2_ps_lighten_blend_func> >	avx2_ps_lighten_blend_hda;

typedef avx2_ps_variation    <avx2_ps_lighten_blend_n>		avx2_ps_lighten_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_lighten_blend_n>		avx2_ps_lighten_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_lighten_blend_hda>	avx2_ps_lighten_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_lighten_blend_hda>	avx2_ps_lighten_blend_hda_o_functor;

//-------------------------------------
// PsDarkenBlend: pre-blend hook で ms = min(ms, md)
struct avx2_ps_darken_blend_func {
	inline void blend( __m128i& md, __m128i& ms ) const {
		__m128i ms2 = ms;
		ms2 = _mm_subs_epu8( ms2, md );
		ms = _mm_sub_epi8( ms, ms2 );
	}
	inline void blend( __m256i& md, __m256i& ms ) const {
		__m256i ms2 = ms;
		ms2 = _mm256_subs_epu8( ms2, md );
		ms = _mm256_sub_epi8( ms, ms2 );
	}
};
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_func,    avx2_ps_darken_blend_func> >	avx2_ps_darken_blend_n;
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_hda_func, avx2_ps_darken_blend_func> >	avx2_ps_darken_blend_hda;

typedef avx2_ps_variation    <avx2_ps_darken_blend_n>		avx2_ps_darken_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_darken_blend_n>		avx2_ps_darken_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_darken_blend_hda>		avx2_ps_darken_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_darken_blend_hda>		avx2_ps_darken_blend_hda_o_functor;

//-------------------------------------
// PsDiffBlend (差の絶対値): packed bytes で diff = |s-d| を計算してから
// alpha_blend に渡す (sub_blend / add_blend と同じ「explicit pattern」)。
template<typename alpha_blend_base>
struct avx2_ps_diff_blend_func : public alpha_blend_base {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i md2 = md;
		md2 = _mm_subs_epu8( md2, ms );		// max(0, d-s)
		ms  = _mm_subs_epu8( ms, md );		// max(0, s-d)
		md2 = _mm_add_epi8( md2, ms );		// |s-d|
		return alpha_blend_base::one( md, md2, ma );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		__m256i md2 = md1;
		md2 = _mm256_subs_epu8( md2, ms1 );
		ms1 = _mm256_subs_epu8( ms1, md1 );
		md2 = _mm256_add_epi8( md2, ms1 );
		return alpha_blend_base::operator()( md1, md2, ma1 );
	}
};
typedef avx2_ps_diff_blend_func<avx2_ps_alpha_blend_func>		avx2_ps_diff_blend_n;
typedef avx2_ps_diff_blend_func<avx2_ps_alpha_blend_hda_func>	avx2_ps_diff_blend_hda;

typedef avx2_ps_variation    <avx2_ps_diff_blend_n>		avx2_ps_diff_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_diff_blend_n>		avx2_ps_diff_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_diff_blend_hda>	avx2_ps_diff_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_diff_blend_hda>	avx2_ps_diff_blend_hda_o_functor;

//-------------------------------------
// PsScreenBlend: c = s + d - (s*d)/255
// 専用テンプレート (alpha_blend テンプレートに hook を渡す形ではなく、
// pre-mul の中間結果が必要なため独自に unpack して計算する)。
template<typename pack_func>
struct avx2_ps_screen_blend {
	const __m128i zero128_;
	const __m256i zero_;
	inline avx2_ps_screen_blend()
		: zero128_( _mm_setzero_si128() ), zero_( _mm256_setzero_si256() ) {}
	inline tjs_uint32 one( __m128i md, __m128i ms, __m128i ma ) const {
		ms = _mm_unpacklo_epi8( ms, zero128_ );
		md = _mm_unpacklo_epi8( md, zero128_ );

		__m128i ms2 = ms;
		ms = _mm_mullo_epi16( ms, md );			// = dst*src
		ms = _mm_srli_epi16( ms, 8 );			// = (s*d)>>8

		ma = pack_func::pack_alpha( ma );

		ms2 = _mm_sub_epi16( ms2, ms );			// src - (s*d/256)
		ms2 = _mm_mullo_epi16( ms2, ma );		// (...)*a
		ms2 = _mm_srai_epi16( ms2, 7 );
		md = _mm_add_epi16( md, ms2 );
		md = _mm_packus_epi16( md, zero128_ );
		return _mm_cvtsi128_si32( md );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		__m256i ms2 = ms1;
		__m256i md2 = md1;

		ms1 = _mm256_unpacklo_epi8( ms1, zero_ );	// (src l)
		md1 = _mm256_unpacklo_epi8( md1, zero_ );	// (dst l)
		__m256i ms12 = ms1;
		ms1 = _mm256_mullo_epi16( ms1, md1 );
		ms1 = _mm256_srli_epi16( ms1, 8 );
		ma1 = pack_func::pack_alpha1( ma1 );
		__m256i ma2 = ma1;
		ma1 = pack_func::pack_alpha12( ma1 );
		ms12 = _mm256_sub_epi16( ms12, ms1 );
		ms12 = _mm256_mullo_epi16( ms12, ma1 );
		ms12 = _mm256_srai_epi16( ms12, 7 );
		md1 = _mm256_add_epi16( md1, ms12 );

		ms2 = _mm256_unpackhi_epi8( ms2, zero_ );	// (src h)
		md2 = _mm256_unpackhi_epi8( md2, zero_ );	// (dst h)
		__m256i ms22 = ms2;
		ms2 = _mm256_mullo_epi16( ms2, md2 );
		ms2 = _mm256_srli_epi16( ms2, 8 );
		ma2 = pack_func::pack_alpha2( ma2 );
		ms22 = _mm256_sub_epi16( ms22, ms2 );
		ms22 = _mm256_mullo_epi16( ms22, ma2 );
		ms22 = _mm256_srai_epi16( ms22, 7 );
		md2 = _mm256_add_epi16( md2, ms22 );
		return _mm256_packus_epi16( md1, md2 );
	}
};
typedef avx2_ps_screen_blend<avx2_ps_pack_func>		avx2_ps_screen_blend_func;
typedef avx2_ps_screen_blend<avx2_ps_pack_hda_func>	avx2_ps_screen_blend_hda_func;

typedef avx2_ps_alpha_blend_f<avx2_ps_screen_blend_func>		avx2_ps_screen_blend_n;
typedef avx2_ps_alpha_blend_f<avx2_ps_screen_blend_hda_func>	avx2_ps_screen_blend_hda;

typedef avx2_ps_variation    <avx2_ps_screen_blend_n>		avx2_ps_screen_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_screen_blend_n>		avx2_ps_screen_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_screen_blend_hda>		avx2_ps_screen_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_screen_blend_hda>		avx2_ps_screen_blend_hda_o_functor;

//-------------------------------------
// PsExclusionBlend: c = s + d - 2*s*d/255
template<typename pack_func>
struct avx2_ps_exclusion_blend_func {
	const __m128i zero128_;
	const __m256i zero_;
	inline avx2_ps_exclusion_blend_func()
		: zero128_( _mm_setzero_si128() ), zero_( _mm256_setzero_si256() ) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		return one( md, ms, ma );
	}
	inline tjs_uint32 one( __m128i md, __m128i ms, __m128i ma ) const {
		md = _mm_unpacklo_epi8( md, zero128_ );
		ms = _mm_unpacklo_epi8( ms, zero128_ );

		__m128i ms2 = ms;
		ms = _mm_mullo_epi16( ms, md );			// dst*src
		ms = _mm_srli_epi16( ms, 7 );			// (d*s*2)/255
		ms2 = _mm_sub_epi16( ms2, ms );			// (d+s) - 2*d*s/255

		ma = pack_func::pack_alpha( ma );
		ms2 = _mm_mullo_epi16( ms2, ma );
		ms2 = _mm_srai_epi16( ms2, 7 );
		md = _mm_add_epi16( md, ms2 );
		md = _mm_packus_epi16( md, zero128_ );
		return _mm_cvtsi128_si32( md );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		__m256i ms2 = ms1;
		__m256i md2 = md1;

		md1 = _mm256_unpacklo_epi8( md1, zero_ );
		ms1 = _mm256_unpacklo_epi8( ms1, zero_ );
		__m256i ms12 = ms1;
		ms1 = _mm256_mullo_epi16( ms1, md1 );
		ms1 = _mm256_srli_epi16( ms1, 7 );
		ms12 = _mm256_sub_epi16( ms12, ms1 );
		ma1 = pack_func::pack_alpha1( ma1 );
		__m256i ma2 = ma1;
		ma1 = pack_func::pack_alpha12( ma1 );
		ms12 = _mm256_mullo_epi16( ms12, ma1 );
		ms12 = _mm256_srai_epi16( ms12, 7 );
		md1 = _mm256_add_epi16( md1, ms12 );

		md2 = _mm256_unpackhi_epi8( md2, zero_ );
		ms2 = _mm256_unpackhi_epi8( ms2, zero_ );
		__m256i ms22 = ms2;
		ms2 = _mm256_mullo_epi16( ms2, md2 );
		ms2 = _mm256_srli_epi16( ms2, 7 );
		ms22 = _mm256_sub_epi16( ms22, ms2 );
		ma2 = pack_func::pack_alpha2( ma2 );
		ms22 = _mm256_mullo_epi16( ms22, ma2 );
		ms22 = _mm256_srai_epi16( ms22, 7 );
		md2 = _mm256_add_epi16( md2, ms22 );
		return _mm256_packus_epi16( md1, md2 );
	}
};
typedef avx2_ps_exclusion_blend_func<avx2_ps_pack_func>		avx2_ps_exclusion_blend_n;
typedef avx2_ps_exclusion_blend_func<avx2_ps_pack_hda_func>	avx2_ps_exclusion_blend_hda;

typedef avx2_ps_variation    <avx2_ps_exclusion_blend_n>	avx2_ps_exclusion_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_exclusion_blend_n>	avx2_ps_exclusion_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_exclusion_blend_hda>	avx2_ps_exclusion_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_exclusion_blend_hda>	avx2_ps_exclusion_blend_hda_o_functor;

//-------------------------------------
// PsDiff5Blend (Photoshop 5.x compat): src を a で先に減衰させてから |s-d|。
//   1. s = (*src) * a
//   2. diff = abs(s - (*dst))
template<typename pack_func>
struct avx2_ps_diff5_blend_func {
	const __m128i zero128_;
	const __m256i zero_;
	inline avx2_ps_diff5_blend_func()
		: zero128_( _mm_setzero_si128() ), zero_( _mm256_setzero_si256() ) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		return one( md, ms, ma );
	}
	inline tjs_uint32 one( __m128i md, __m128i ms, __m128i ma ) const {
		ms = _mm_unpacklo_epi8( ms, zero128_ );
		md = _mm_unpacklo_epi8( md, zero128_ );
		ma = pack_func::pack_alpha( ma );

		ms = _mm_mullo_epi16( ms, ma );
		__m128i md2 = md;
		ms = _mm_srai_epi16( ms, 7 );
		md2 = _mm_subs_epu16( md2, ms );		// dst - src*a (sat)
		ms = _mm_subs_epu16( ms, md );			// src*a - dst (sat)
		md2 = _mm_add_epi16( md2, ms );			// |...|
		md2 = _mm_packus_epi16( md2, zero128_ );
		return _mm_cvtsi128_si32( md2 );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		__m256i ms2 = ms1;
		__m256i md2 = md1;

		ms1 = _mm256_unpacklo_epi8( ms1, zero_ );
		md1 = _mm256_unpacklo_epi8( md1, zero_ );
		ma1 = pack_func::pack_alpha1( ma1 );
		__m256i ma2 = ma1;
		ma1 = pack_func::pack_alpha12( ma1 );
		ms1 = _mm256_mullo_epi16( ms1, ma1 );
		__m256i md12 = md1;
		ms1 = _mm256_srai_epi16( ms1, 7 );
		md12 = _mm256_subs_epu16( md12, ms1 );
		ms1 = _mm256_subs_epu16( ms1, md1 );
		md12 = _mm256_add_epi16( md12, ms1 );

		ms2 = _mm256_unpackhi_epi8( ms2, zero_ );
		md2 = _mm256_unpackhi_epi8( md2, zero_ );
		ma2 = pack_func::pack_alpha2( ma2 );
		ms2 = _mm256_mullo_epi16( ms2, ma2 );
		__m256i md22 = md2;
		ms2 = _mm256_srai_epi16( ms2, 7 );
		md22 = _mm256_subs_epu16( md22, ms2 );
		ms2 = _mm256_subs_epu16( ms2, md2 );
		md22 = _mm256_add_epi16( md22, ms2 );
		return _mm256_packus_epi16( md12, md22 );
	}
};
typedef avx2_ps_diff5_blend_func<avx2_ps_pack_func>		avx2_ps_diff5_blend_n;
typedef avx2_ps_diff5_blend_func<avx2_ps_pack_hda_func>	avx2_ps_diff5_blend_hda;

typedef avx2_ps_variation    <avx2_ps_diff5_blend_n>	avx2_ps_diff5_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_diff5_blend_n>	avx2_ps_diff5_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_diff5_blend_hda>	avx2_ps_diff5_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_diff5_blend_hda>	avx2_ps_diff5_blend_hda_o_functor;

//-------------------------------------
// PsOverlayBlend: dst < 128 → 2*s*d/255、dst >= 128 → 1 - 2*(1-s)*(1-d)/255
// pre-blend hook で両方計算してから dst の閾値で select する。
struct avx2_ps_overlay_blend_func {
	const __m128i mask128_;
	const __m256i mask_;
	inline avx2_ps_overlay_blend_func()
		: mask128_(_mm_set1_epi16(0x00FF)), mask_(_mm256_set1_epi16(0x00FF)) {}
	inline void blend( __m128i& md, __m128i& ms ) const {
		__m128i ms2 = ms;
		ms = _mm_mullo_epi16( ms, md );		// dst*src
		ms2 = _mm_add_epi16( ms2, md );		// dst+src
		ms = _mm_srli_epi16( ms, 7 );		// 2*s*d/256
		ms2 = _mm_slli_epi16( ms2, 1 );		// (d+s)*2
		__m128i threshold = _mm_set1_epi16(0x0080);
		ms2 = _mm_sub_epi16( ms2, ms );
		ms2 = _mm_sub_epi16( ms2, mask128_ );
		threshold = _mm_cmpgt_epi16( threshold, md );	// (128 > d) ? 0xffff : 0
		ms = _mm_and_si128( ms, threshold );
		threshold = _mm_andnot_si128( threshold, ms2 );
		ms = _mm_or_si128( ms, threshold );
	}
	inline void blend( __m256i& md, __m256i& ms ) const {
		__m256i ms2 = ms;
		ms = _mm256_mullo_epi16( ms, md );
		ms2 = _mm256_add_epi16( ms2, md );
		ms = _mm256_srli_epi16( ms, 7 );
		ms2 = _mm256_slli_epi16( ms2, 1 );
		__m256i threshold = _mm256_set1_epi16(0x0080);
		ms2 = _mm256_sub_epi16( ms2, ms );
		ms2 = _mm256_sub_epi16( ms2, mask_ );
		threshold = _mm256_cmpgt_epi16( threshold, md );
		ms = _mm256_and_si256( ms, threshold );
		threshold = _mm256_andnot_si256( threshold, ms2 );
		ms = _mm256_or_si256( ms, threshold );
	}
};
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_func,    avx2_ps_overlay_blend_func> >	avx2_ps_overlay_blend_n;
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_hda_func, avx2_ps_overlay_blend_func> >	avx2_ps_overlay_blend_hda;

typedef avx2_ps_variation    <avx2_ps_overlay_blend_n>		avx2_ps_overlay_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_overlay_blend_n>		avx2_ps_overlay_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_overlay_blend_hda>	avx2_ps_overlay_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_overlay_blend_hda>	avx2_ps_overlay_blend_hda_o_functor;

//-------------------------------------
// PsHardLightBlend: Overlay と同じ条件分岐だが src 側で判定する。
struct avx2_ps_hardlight_blend_func {
	const __m128i mask128_;
	const __m256i mask_;
	inline avx2_ps_hardlight_blend_func()
		: mask128_(_mm_set1_epi16(0x00FF)), mask_(_mm256_set1_epi16(0x00FF)) {}
	inline void blend( __m128i& md, __m128i& ms ) const {
		__m128i threshold = _mm_set1_epi16(0x0080);
		__m128i ms2 = ms;
		threshold = _mm_cmpgt_epi16( threshold, ms2 );	// (128 > s) ? 0xffff : 0
		ms2 = _mm_add_epi16( ms2, md );
		ms = _mm_mullo_epi16( ms, md );
		ms2 = _mm_slli_epi16( ms2, 1 );
		ms = _mm_srli_epi16( ms, 7 );
		ms2 = _mm_sub_epi16( ms2, ms );
		ms2 = _mm_sub_epi16( ms2, mask128_ );
		ms = _mm_and_si128( ms, threshold );
		threshold = _mm_andnot_si128( threshold, ms2 );
		ms = _mm_or_si128( ms, threshold );
	}
	inline void blend( __m256i& md, __m256i& ms ) const {
		__m256i threshold = _mm256_set1_epi16(0x0080);
		__m256i ms2 = ms;
		threshold = _mm256_cmpgt_epi16( threshold, ms2 );
		ms2 = _mm256_add_epi16( ms2, md );
		ms = _mm256_mullo_epi16( ms, md );
		ms2 = _mm256_slli_epi16( ms2, 1 );
		ms = _mm256_srli_epi16( ms, 7 );
		ms2 = _mm256_sub_epi16( ms2, ms );
		ms2 = _mm256_sub_epi16( ms2, mask_ );
		ms = _mm256_and_si256( ms, threshold );
		threshold = _mm256_andnot_si256( threshold, ms2 );
		ms = _mm256_or_si256( ms, threshold );
	}
};
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_func,    avx2_ps_hardlight_blend_func> >	avx2_ps_hardlight_blend_n;
typedef avx2_ps_alpha_blend_f<avx2_ps_alpha_blend<avx2_ps_pack_hda_func, avx2_ps_hardlight_blend_func> >	avx2_ps_hardlight_blend_hda;

typedef avx2_ps_variation    <avx2_ps_hardlight_blend_n>	avx2_ps_hardlight_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_hardlight_blend_n>	avx2_ps_hardlight_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_hardlight_blend_hda>	avx2_ps_hardlight_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_hardlight_blend_hda>	avx2_ps_hardlight_blend_hda_o_functor;

//-------------------------------------
// テーブル lookup 系 (SoftLight / ColorDodge / ColorBurn): pre-blend として
// 全チャネルを TTable[s][d] で置換してから alpha_blend に渡す。SIMD で
// 表引きするのは現実的でないので scalar loop で 8 pixel 分処理する。
template<typename TTable, typename alpha_blend_base>
struct avx2_ps_table_blend_func : public alpha_blend_base {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		s = (TTable::TABLE[(s>>16)&0xff][(d>>16)&0xff]<<16) |
			(TTable::TABLE[(s>>8 )&0xff][(d>>8 )&0xff]<<8 ) |
			(TTable::TABLE[(s>>0 )&0xff][(d>>0 )&0xff]<<0 );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i md = _mm_cvtsi32_si128( d );
		return alpha_blend_base::one( md, ms, ma );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		alignas(32) tjs_uint32 d_arr[8], s_arr[8];
		_mm256_store_si256( (__m256i*)d_arr, md1 );
		_mm256_store_si256( (__m256i*)s_arr, ms1 );
		for (int i = 0; i < 8; ++i) {
			tjs_uint32 d = d_arr[i];
			tjs_uint32 s = s_arr[i];
			s_arr[i] = (TTable::TABLE[(s>>16)&0xff][(d>>16)&0xff]<<16) |
			           (TTable::TABLE[(s>>8 )&0xff][(d>>8 )&0xff]<<8 ) |
			           (TTable::TABLE[(s>>0 )&0xff][(d>>0 )&0xff]<<0 );
		}
		__m256i ms_new = _mm256_load_si256( (const __m256i*)s_arr );
		return alpha_blend_base::operator()( md1, ms_new, ma1 );
	}
};

typedef avx2_ps_table_blend_func<ps_soft_light_table, avx2_ps_alpha_blend_func>		avx2_ps_softlight_blend_n;
typedef avx2_ps_table_blend_func<ps_soft_light_table, avx2_ps_alpha_blend_hda_func>	avx2_ps_softlight_blend_hda;

typedef avx2_ps_variation    <avx2_ps_softlight_blend_n>	avx2_ps_softlight_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_softlight_blend_n>	avx2_ps_softlight_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_softlight_blend_hda>	avx2_ps_softlight_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_softlight_blend_hda>	avx2_ps_softlight_blend_hda_o_functor;

typedef avx2_ps_table_blend_func<ps_color_dodge_table, avx2_ps_alpha_blend_func>	avx2_ps_colordodge_blend_n;
typedef avx2_ps_table_blend_func<ps_color_dodge_table, avx2_ps_alpha_blend_hda_func>	avx2_ps_colordodge_blend_hda;

typedef avx2_ps_variation    <avx2_ps_colordodge_blend_n>		avx2_ps_colordodge_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_colordodge_blend_n>		avx2_ps_colordodge_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_colordodge_blend_hda>		avx2_ps_colordodge_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_colordodge_blend_hda>		avx2_ps_colordodge_blend_hda_o_functor;

typedef avx2_ps_table_blend_func<ps_color_burn_table, avx2_ps_alpha_blend_func>		avx2_ps_colorburn_blend_n;
typedef avx2_ps_table_blend_func<ps_color_burn_table, avx2_ps_alpha_blend_hda_func>	avx2_ps_colorburn_blend_hda;

typedef avx2_ps_variation    <avx2_ps_colorburn_blend_n>		avx2_ps_colorburn_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_colorburn_blend_n>		avx2_ps_colorburn_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_colorburn_blend_hda>		avx2_ps_colorburn_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_colorburn_blend_hda>		avx2_ps_colorburn_blend_hda_o_functor;

//-------------------------------------
// PsColorDodge5Blend (Photoshop 5.x compat): src を a で先に減衰させてから
// テーブル参照する Photoshop 5.x 互換版。alpha_blend を経由しない。
template<typename pack_func>
struct avx2_ps_colordodge5_blend_func {
	const __m128i zero128_;
	inline avx2_ps_colordodge5_blend_func() : zero128_( _mm_setzero_si128() ) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		ma = pack_func::pack_alpha( ma );
		ms = _mm_unpacklo_epi8( ms, zero128_ );
		ms = _mm_mullo_epi16( ms, ma );
		ms = _mm_srai_epi16( ms, 7 );
		ms = _mm_packus_epi16( ms, zero128_ );
		s = _mm_cvtsi128_si32( ms );
		return (ps_color_dodge_table::TABLE[(s>>16)&0xff][(d>>16)&0xff]<<16) |
		       (ps_color_dodge_table::TABLE[(s>>8 )&0xff][(d>>8 )&0xff]<<8 ) |
		       (ps_color_dodge_table::TABLE[(s>>0 )&0xff][(d>>0 )&0xff]<<0 );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		// 1 段目: src を a で減衰させる (sse2 と同じく低位/高位 4 ピクセルを
		// 別々に unpack → mullo → srai → packus)。
		const __m256i zero = _mm256_setzero_si256();
		__m256i ms2 = ms1;
		ma1 = pack_func::pack_alpha1( ma1 );
		__m256i ma2 = ma1;
		ma1 = pack_func::pack_alpha12( ma1 );

		ms1 = _mm256_unpacklo_epi8( ms1, zero );
		ms1 = _mm256_mullo_epi16( ms1, ma1 );
		ms1 = _mm256_srai_epi16( ms1, 7 );

		ma2 = pack_func::pack_alpha2( ma2 );
		ms2 = _mm256_unpackhi_epi8( ms2, zero );
		ms2 = _mm256_mullo_epi16( ms2, ma2 );
		ms2 = _mm256_srai_epi16( ms2, 7 );
		__m256i s_pmul = _mm256_packus_epi16( ms1, ms2 );

		// 2 段目: テーブル参照 (8 pixel 分の scalar lookup)
		alignas(32) tjs_uint32 d_arr[8], s_arr[8];
		_mm256_store_si256( (__m256i*)d_arr, md1 );
		_mm256_store_si256( (__m256i*)s_arr, s_pmul );
		for (int i = 0; i < 8; ++i) {
			tjs_uint32 d = d_arr[i];
			tjs_uint32 s = s_arr[i];
			s_arr[i] = (ps_color_dodge_table::TABLE[(s>>16)&0xff][(d>>16)&0xff]<<16) |
			           (ps_color_dodge_table::TABLE[(s>>8 )&0xff][(d>>8 )&0xff]<<8 ) |
			           (ps_color_dodge_table::TABLE[(s>>0 )&0xff][(d>>0 )&0xff]<<0 );
		}
		return _mm256_load_si256( (const __m256i*)s_arr );
	}
};
// HDA 版は dst の alpha を OR して preserve する wrapper
struct avx2_ps_colordodge5_blend_hda_func : public avx2_ps_colordodge5_blend_func<avx2_ps_pack_func> {
	typedef avx2_ps_colordodge5_blend_func<avx2_ps_pack_func> base;
	const __m256i alphamask_;
	inline avx2_ps_colordodge5_blend_hda_func() : alphamask_(_mm256_set1_epi32(0xFF000000)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		return base::operator()( d, s, a ) | (d & 0xff000000);
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		__m256i mda = _mm256_and_si256( md1, alphamask_ );
		__m256i ret = base::operator()( md1, ms1, ma1 );
		return _mm256_or_si256( ret, mda );
	}
};
typedef avx2_ps_colordodge5_blend_func<avx2_ps_pack_func>	avx2_ps_colordodge5_blend_n;
typedef avx2_ps_colordodge5_blend_hda_func					avx2_ps_colordodge5_blend_hda;

typedef avx2_ps_variation    <avx2_ps_colordodge5_blend_n>		avx2_ps_colordodge5_blend_functor;
typedef avx2_ps_variation_opa<avx2_ps_colordodge5_blend_n>		avx2_ps_colordodge5_blend_o_functor;
typedef avx2_ps_variation    <avx2_ps_colordodge5_blend_hda>	avx2_ps_colordodge5_blend_hda_functor;
typedef avx2_ps_variation_opa<avx2_ps_colordodge5_blend_hda>	avx2_ps_colordodge5_blend_hda_o_functor;

#endif // __BLEND_PS_FUNCTOR_AVX2_H__
