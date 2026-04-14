
#ifndef __BLEND_FUNCTOR_AVX2_H__
#define __BLEND_FUNCTOR_AVX2_H__

#include "x86simdutil.h"

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[256*256];
extern unsigned char TVPNegativeMulTable[256*256];
};

// ソースのアルファを使う
// 非 HDA バリアントなので結果のアルファは 0 にする (C リファレンス互換)
template<typename blend_func>
struct avx2_variation : public blend_func {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 a = (s>>24);
		return blend_func::operator()( d, s, a ) & 0x00ffffff;
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		__m256i a = s;
		a = _mm256_srli_epi32( a, 24 );
		return _mm256_and_si256( blend_func::operator()( d, s, a ),
		                          _mm256_set1_epi32( 0x00ffffff ) );
	}
};

// ソースのアルファとopacity値を使う
// 非 HDA バリアントなので結果のアルファは 0 にする (C リファレンス互換)
template<typename blend_func>
struct avx2_variation_opa : public blend_func {
	const tjs_int32 opa_;
	const __m256i opa256_;
	const __m256i colormask_;
	inline avx2_variation_opa( tjs_int32 opa )
		: opa_(opa), opa256_(_mm256_set1_epi32(opa)),
		  colormask_(_mm256_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		// 旧実装の `(s*opa) >> 32` は s.G/B からの繰り上がりが bit32 に乗ると
		// (sa*opa)>>8 と±1ズレることがあった (C リファレンスと不一致)。
		tjs_uint32 a = ((s >> 24) * opa_) >> 8;
		return blend_func::operator()( d, s, a ) & 0x00ffffff;
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		__m256i a = s;
		a = _mm256_srli_epi32( a, 24 );
		a = _mm256_mullo_epi16( a, opa256_ );
		a = _mm256_srli_epi32( a, 8 );
		return _mm256_and_si256( blend_func::operator()( d, s, a ), colormask_ );
	}
};

// ソースとデスティネーションのアルファを使う
struct avx2_alpha_blend_d_functor {
	const __m256i m255_;
	const __m256i zero_;
	const __m256i colormask_;
	const __m256 m65535_;
	inline avx2_alpha_blend_d_functor() : m255_(_mm256_set1_epi32(255)), m65535_(_mm256_set1_ps(65535.0f)),
		zero_(_mm256_setzero_si256()), colormask_(_mm256_set1_epi32(0x00ffffff)) {}

	// 旧実装は AVX rcp_ps による 1/ca 近似で最大誤差 8 を許容していたが、
	// パリティテストとの byte-exact 互換のため C リファレンスと同じ
	// TVPOpacityOnOpacityTable / TVPNegativeMulTable 直接参照に変更。
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 addr = ((s >> 16) & 0xff00) + (d>>24);
		tjs_uint32 sopa = TVPOpacityOnOpacityTable[addr];

		__m128i ma = _mm_cvtsi32_si128( sopa );
		ma = _mm_shufflelo_epi16( ma, _MM_SHUFFLE( 0, 0, 0, 0 )  );	// 00oo00oo00oo00oo
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_cvtepu8_epi16( ms );// 00 ss 00 ss 00 ss 00 ss
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_cvtepu8_epi16( md );// 00 dd 00 dd 00 dd 00 dd

		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, ma );		// ms *= ma
		md = _mm_slli_epi16( md, 8 );		// md <<= 8
		md = _mm_add_epi16( md, ms );		// md += ms
		md = _mm_srli_epi16( md, 8 );		// ms >>= 8
		md = _mm_packus_epi16( md, md );
		tjs_uint32 ret = _mm_cvtsi128_si32( md );

		addr = TVPNegativeMulTable[addr] << 24;
		return (ret&0x00ffffff) | addr;
	}
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const {
		// addr = ((s>>16)&0xff00) + (d>>24) per 32-bit lane
		__m256i sa = _mm256_srli_epi32( ms1, 24 );
		__m256i da = _mm256_srli_epi32( md1, 24 );
		__m256i maddr = _mm256_or_si256( _mm256_slli_epi32( sa, 8 ), da );

		// 8 pixel ぶん TVPOpacityOnOpacityTable を直接引く (C ref と byte-exact)
		__m256i ma = _mm256_set_epi32(
			TVPOpacityOnOpacityTable[M256I_U32(maddr,7)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,6)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,5)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,4)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,3)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,2)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,1)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,0)] );

		// AVX2 の 256bit unpack は 128bit lane 内で動く: lane0=pixel 0..3, lane1=pixel 4..7
		// 各 128bit lane 内で sopa を 4x16bit に broadcast する
		ma = _mm256_packs_epi32( ma, ma );			// lane 内: [s0,s1,s2,s3,s0,s1,s2,s3]
		ma = _mm256_unpacklo_epi16( ma, ma );		// lane 内: [s0,s0,s1,s1,s2,s2,s3,s3]
		__m256i ma2 = ma;
		ma  = _mm256_unpacklo_epi16( ma,  ma  );	// lane 内 lo: pixel 0,1
		ma2 = _mm256_unpackhi_epi16( ma2, ma2 );	// lane 内 hi: pixel 2,3

		// d + ((s-d)*sopa >> 8) per channel
		__m256i ms_lo = _mm256_unpacklo_epi8( ms1, zero_ );
		__m256i md_lo = _mm256_unpacklo_epi8( md1, zero_ );
		ms_lo = _mm256_sub_epi16( ms_lo, md_lo );
		ms_lo = _mm256_mullo_epi16( ms_lo, ma );
		md_lo = _mm256_slli_epi16( md_lo, 8 );
		md_lo = _mm256_add_epi16( md_lo, ms_lo );
		md_lo = _mm256_srli_epi16( md_lo, 8 );

		__m256i ms_hi = _mm256_unpackhi_epi8( ms1, zero_ );
		__m256i md_hi = _mm256_unpackhi_epi8( md1, zero_ );
		ms_hi = _mm256_sub_epi16( ms_hi, md_hi );
		ms_hi = _mm256_mullo_epi16( ms_hi, ma2 );
		md_hi = _mm256_slli_epi16( md_hi, 8 );
		md_hi = _mm256_add_epi16( md_hi, ms_hi );
		md_hi = _mm256_srli_epi16( md_hi, 8 );

		__m256i packed = _mm256_packus_epi16( md_lo, md_hi );

		// dest alpha = TVPNegativeMulTable[addr]
		__m256i mneg = _mm256_set_epi32(
			TVPNegativeMulTable[M256I_U32(maddr,7)],
			TVPNegativeMulTable[M256I_U32(maddr,6)],
			TVPNegativeMulTable[M256I_U32(maddr,5)],
			TVPNegativeMulTable[M256I_U32(maddr,4)],
			TVPNegativeMulTable[M256I_U32(maddr,3)],
			TVPNegativeMulTable[M256I_U32(maddr,2)],
			TVPNegativeMulTable[M256I_U32(maddr,1)],
			TVPNegativeMulTable[M256I_U32(maddr,0)] );
		mneg = _mm256_slli_epi32( mneg, 24 );

		packed = _mm256_and_si256( packed, colormask_ );
		return _mm256_or_si256( packed, mneg );
	}
};

template<typename blend_func>
struct avx2_variation_hda : public blend_func {
	__m256i alphamask_;
	__m256i colormask_;
	inline avx2_variation_hda() {
		alphamask_ = _mm256_set1_epi32( 0xFF000000 );
		colormask_ = _mm256_set1_epi32( 0x00FFFFFF );
	}
	inline avx2_variation_hda( tjs_int32 opa ) : blend_func(opa) {
		alphamask_ = _mm256_set1_epi32( 0xFF000000 );
		colormask_ = _mm256_set1_epi32( 0x00FFFFFF );
	}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 dstalpha = d&0xff000000;
		tjs_uint32 ret = blend_func::operator()( d, s );
		return (ret&0x00ffffff)|dstalpha;
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		__m256i dstalpha = _mm256_and_si256( d, alphamask_ );
		__m256i ret = blend_func::operator()( d, s );
		ret = _mm256_and_si256( ret, colormask_ );
		return _mm256_or_si256( ret, dstalpha );
	}
};

struct avx2_alpha_blend_func {
	const __m256i zero_;
	inline avx2_alpha_blend_func() : zero_( _mm256_setzero_si256() ) {}
	inline tjs_uint32 one( __m128i md, __m128i ms, __m128i ma ) const {
		ms = _mm_cvtepu8_epi16( ms );// 00 ss 00 ss 00 ss 00 ss
		md = _mm_cvtepu8_epi16( md );// 00 dd 00 dd 00 dd 00 dd
		ma = _mm_shufflelo_epi16( ma, _MM_SHUFFLE( 0, 0, 0, 0 )  );	// 00aa00aa00aa00aa
		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, ma );		// ms *= ma
		ms = _mm_srli_epi16( ms, 8 );		// ms >>= 8
		md = _mm_add_epi8( md, ms );		// md += ms : d + ((s-d)*sopa)>>8
		md = _mm_packus_epi16( md, md );	// pack
		return _mm_cvtsi128_si32( md );		// store
	}
#if 0
	// こっちは遅い。zeroを省けるがそれ以上に遅い。
	inline __m256i operator()( __m256i md, __m256i ms, __m256i ma1 ) const {
		__m128i ms1s = _mm256_extracti128_si256( ms, 0 );
		__m128i md1s = _mm256_extracti128_si256( md, 0 );
		__m256i ms1 = _mm256_cvtepu8_epi16( ms1s );	// 00ss00ss00ss00ss
		__m256i md1 = _mm256_cvtepu8_epi16( md1s );	// 00dd00dd00dd00dd

		ma1 = _mm256_packs_epi32( ma1, ma1 );		// 4 5 6 7 4 5 6 7 | 0 1 2 3 0 1 2 3
		ma1 = _mm256_unpacklo_epi16( ma1, ma1 );	// 4 4 5 5 6 6 7 7 | 0 0 1 1 2 2 3 3
		__m256i ma2 = ma1;
		ma1 = _mm256_permute4x64_epi64( ma1, (1 << 6) | (1 << 4) | (0 << 2) | (0 << 0) );	// 00 11 00 11 | 22 33 22 33
		ma2 = _mm256_permute4x64_epi64( ma2, (3 << 6) | (3 << 4) | (2 << 2) | (2 << 0) );
		ma1 = _mm256_unpacklo_epi16( ma1, ma1 );	// 0000 1111 2222 3333
		ma2 = _mm256_unpacklo_epi16( ma2, ma2 );	// 4444 5555 6666 7777

		ms1 = _mm256_sub_epi16( ms1, md1 );		// s -= d
		ms1 = _mm256_mullo_epi16( ms1, ma1 );	// s *= a
		ms1 = _mm256_srli_epi16( ms1, 8 );		// s >>= 8
		md1 = _mm256_add_epi8( md1, ms1 );		// d += s

		__m128i ms2s = _mm256_extracti128_si256( ms, 1 );
		__m128i md2s = _mm256_extracti128_si256( md, 1 );
		__m256i ms2 = _mm256_cvtepu8_epi16( ms2s );	// 00ss00ss00ss00ss
		__m256i md2 = _mm256_cvtepu8_epi16( md2s );	// 00dd00dd00dd00dd
		ms2 = _mm256_sub_epi16( ms2, md2 );		// s -= d
		ms2 = _mm256_mullo_epi16( ms2, ma2 );	// s *= a
		ms2 = _mm256_srli_epi16( ms2, 8 );		// s >>= 8
		md2 = _mm256_add_epi8( md2, ms2 );		// d += s

		md1 = _mm256_packus_epi16( md1, md2 );	// 0 2 1 3
		return _mm256_permute4x64_epi64( md1, (3 << 6) | (1 << 4) | (2 << 2) | (0 << 0) );
	}
#endif
	// zero を使ってunpack する方が段違いに速い
	inline __m256i operator()( __m256i md, __m256i ms, __m256i ma1 ) const {
		__m256i ms1 = _mm256_unpacklo_epi8( ms, zero_ );
		__m256i md1 = _mm256_unpacklo_epi8( md, zero_ );
		ma1 = _mm256_packs_epi32( ma1, ma1 );			// 4 5 6 7 4 5 6 7 | 0 1 2 3 0 1 2 3
		ma1 = _mm256_unpacklo_epi16( ma1, ma1 );		// 4 4 5 5 6 6 7 7 | 0 0 1 1 2 2 3 3
		__m256i ma2 = _mm256_unpackhi_epi16( ma1, ma1 );// 6 6 6 6 7 7 7 7 | 2 2 2 2 3 3 3 3
		ma1 = _mm256_unpacklo_epi16( ma1, ma1 );		// 4 4 4 4 5 5 5 5 | 0 0 0 0 1 1 1 1
		ms1 = _mm256_sub_epi16( ms1, md1 );		// s -= d
		ms1 = _mm256_mullo_epi16( ms1, ma1 );	// s *= a
		ms1 = _mm256_srli_epi16( ms1, 8 );		// s >>= 8
		md1 = _mm256_add_epi8( md1, ms1 );		// d += s
		__m256i ms2 = _mm256_unpackhi_epi8( ms, zero_ );
		__m256i md2 = _mm256_unpackhi_epi8( md, zero_ );
		ms2 = _mm256_sub_epi16( ms2, md2 );		// s -= d
		ms2 = _mm256_mullo_epi16( ms2, ma2 );	// s *= a
		ms2 = _mm256_srli_epi16( ms2, 8 );		// s >>= 8
		md2 = _mm256_add_epi8( md2, ms2 );		// d += s
		return _mm256_packus_epi16( md1, md2 );	// 4567 0123
	}
};

struct avx2_alpha_blend : public avx2_alpha_blend_func {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		return avx2_alpha_blend_func::one( md, ms, ma );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1, __m256i ma1 ) const {
		return avx2_alpha_blend_func::operator()( md1, ms1, ma1 );
	}
};
// もっともシンプルなコピー dst = src
struct avx2_const_copy_functor {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const { return s; }
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const { return ms1; }
};
// 単純コピーだけど alpha をコピーしない(HDAと同じ)
struct avx2_color_copy_functor {
	const __m256i colormask_;
	const __m256i alphamask_;
	inline avx2_color_copy_functor() : colormask_(_mm256_set1_epi32(0x00ffffff)), alphamask_(_mm256_set1_epi32(0xff000000)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		return (d&0xff000000) + (s&0x00ffffff);
	}
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const {
		ms1 = _mm256_and_si256( ms1, colormask_ );
		md1 = _mm256_and_si256( md1, alphamask_ );
		return _mm256_or_si256( md1, ms1 );
	}
};
// alphaだけコピーする : color_copy の src destを反転しただけ
struct avx2_alpha_copy_functor : public avx2_color_copy_functor {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		return avx2_color_copy_functor::operator()( s, d );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const {
		return avx2_color_copy_functor::operator()( ms1, md1 );
	}
};
// このままコピーするがアルファを0xffで埋める dst = 0xff000000 | src
struct avx2_color_opaque_functor {
	const __m256i alphamask_;
	inline avx2_color_opaque_functor() : alphamask_(_mm256_set1_epi32(0xff000000)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const { return 0xff000000 | s; }
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const { return _mm256_or_si256( alphamask_, ms1 ); }
};

// 矩形版未実装
struct avx2_alpha_blend_a_functor {
	const __m256i mask_;
	const __m256i zero_;
	inline avx2_alpha_blend_a_functor() : zero_(_mm256_setzero_si256()),
		mask_(_mm256_set_epi32(0x0000ffff,0xffffffff,0x0000ffff,0xffffffff,0x0000ffff,0xffffffff,0x0000ffff,0xffffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		const __m128i mask = _mm256_extracti128_si256( mask_, 0 );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i mo = ms;
		mo = _mm_srli_epi32( mo, 24 );			// >> 24
		__m128i msa = mo;
		msa = _mm_slli_epi64( msa, 48 );		// << 48
		mo = _mm_shufflelo_epi16( mo, _MM_SHUFFLE( 0, 0, 0, 0 )  );	// 00oo00oo00oo00oo
		ms = _mm_cvtepu8_epi16( ms );			// 00Sa00Si00Si00Si SSE4.1
		ms = _mm_mullo_epi16( ms, mo );			// src * sopa
		ms = _mm_srli_epi16( ms, 8 );			// src * sopa >> 8
		ms = _mm_and_si128( ms, mask );			// drop alpha
		ms = _mm_or_si128( ms, msa );			// set original alpha

		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_cvtepu8_epi16( md );		// 00Da00Di00Di00Di SSE4.1
		__m128i md2 = md;
		md2 = _mm_mullo_epi16( md2, mo );	// d * sopa
		md2 = _mm_srli_epi16( md2, 8 );		// 00 SaDa 00 SaDi 00 SaDi 00 SaDi
		md = _mm_sub_epi16( md, md2 );		// d - d*sopa
		md = _mm_add_epi16( md, ms );		// (d-d*sopa) + s
		md = _mm_packus_epi16( md, md );
		return _mm_cvtsi128_si32( md );
	}
	inline __m256i operator()( __m256i md, __m256i ms ) const {
		__m256i mo0 = ms;
		mo0 = _mm256_srli_epi32( mo0, 24 );
		__m256i msa = mo0;
		msa = _mm256_slli_epi32( msa, 24 );		// << 24
		mo0 = _mm256_packs_epi32( mo0, mo0 );		// 0 1 2 3 0 1 2 3
		mo0 = _mm256_unpacklo_epi16( mo0, mo0 );	// 0 0 1 1 2 2 3 3
		__m256i mo1 = mo0;
		mo1 = _mm256_unpacklo_epi16( mo1, mo1 );	// 0 0 0 0 1 1 1 1 o[1]
		mo0 = _mm256_unpackhi_epi16( mo0, mo0 );	// 2 2 2 2 3 3 3 3 o[0]
		
		__m256i ms1 = ms;
		ms = _mm256_unpackhi_epi8( ms, zero_ );
		ms = _mm256_mullo_epi16( ms, mo0 );		// src * sopa
		ms = _mm256_srli_epi16( ms, 8 );			// src * sopa >> 8
		ms = _mm256_and_si256( ms, mask_ );		// drop alpha
		__m256i msa1 = msa;
		msa = _mm256_unpackhi_epi8( msa, zero_ );
		ms = _mm256_or_si256( ms, msa );			// set original alpha
		
		ms1 = _mm256_unpacklo_epi8( ms1, zero_ );
		ms1 = _mm256_srli_epi16( ms1, 8 );			// src * sopa >> 8
		ms1 = _mm256_and_si256( ms1, mask_ );		// drop alpha
		msa1 = _mm256_unpacklo_epi8( msa1, zero_ );
		ms1 = _mm256_or_si256( ms1, msa1 );		// set original alpha

		__m256i md1 = md;
		md = _mm256_unpackhi_epi8( md, zero_ );// 00dd00dd00dd00dd d[0]
		__m256i md02 = md;
		md02 = _mm256_mullo_epi16( md02, mo0 );	// d * sopa | d[0]
		md02 = _mm256_srli_epi16( md02, 8 );	// 00 SaDa 00 SaDi 00 SaDi 00 SaDi | d[0]
		md = _mm256_sub_epi16( md, md02 );		// d - d*sopa | d[0]
		md = _mm256_add_epi16( md, ms );		// d - d*sopa + s | d[0]

		md1 = _mm256_unpacklo_epi8( md1, zero_ );// 00dd00dd00dd00dd d[1]
		__m256i md12 = md1;
		md12 = _mm256_mullo_epi16( md12, mo1 );// d * sopa | d[1]
		md12 = _mm256_srli_epi16( md12, 8 );	// 00 SaDa 00 SaDi 00 SaDi 00 SaDi | d[1]
		md1 = _mm256_sub_epi16( md1, md12 );	// d - d*sopa | d[1]
		md1 = _mm256_add_epi16( md1, ms1 );	// d - d*sopa + s | d[1]

		return _mm256_packus_epi16( md1, md );
	}
};

typedef avx2_variation<avx2_alpha_blend>							avx2_alpha_blend_functor;
typedef avx2_variation_opa<avx2_alpha_blend>						avx2_alpha_blend_o_functor;
typedef avx2_variation_hda<avx2_variation<avx2_alpha_blend> >		avx2_alpha_blend_hda_functor;
typedef avx2_variation_hda<avx2_variation_opa<avx2_alpha_blend> >	avx2_alpha_blend_hda_o_functor;
// avx2_alpha_blend_d_functor
// avx2_alpha_blend_a_functor

// 非 HDA バリアント: C リファレンスは alpha レーンに src.alpha を出力する
// C リファレンス (premulalpha_blend_n_a_func):
//   sopa       = 255 - sa
//   result_rgb = sat_add( (d * sopa) >> 8, s_rgb )
//   result_a   = sa            (非 HDA バリアントは src のアルファをそのまま)
// 旧 AVX2 は SSE2 と同じく `d - (d*sa>>8)` 減算ベースで±1ズレた。
struct avx2_premul_alpha_blend_functor {
	const __m256i zero_;
	const __m256i alphamask_;
	const __m256i colormask_;
	const __m256i m255_;
	inline avx2_premul_alpha_blend_functor()
		: zero_( _mm256_setzero_si256() ),
		  alphamask_(_mm256_set1_epi32(0xff000000)),
		  colormask_(_mm256_set1_epi32(0x00ffffff)),
		  m255_(_mm256_set1_epi16(255)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i ms = _mm_cvtsi32_si128( s );
		tjs_int32 sopa = 255 - (tjs_int32)(s >> 24);	// 255 - sa
		__m128i mo = _mm_cvtsi32_si128( sopa );
		__m128i md = _mm_cvtsi32_si128( d );
		mo = _mm_shufflelo_epi16( mo, _MM_SHUFFLE( 0, 0, 0, 0 ) );
		md = _mm_cvtepu8_epi16( md );
		md = _mm_mullo_epi16( md, mo );			// d * (255 - sa)
		md = _mm_srli_epi16( md, 8 );			// >> 8
		md = _mm_packus_epi16( md, md );
		md = _mm_adds_epu8( md, ms );			// (d*(255-sa))>>8 + src (sat add)
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (s & 0xff000000);
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		__m256i s_orig = s;

		__m256i ma1 = s;
		ma1 = _mm256_srli_epi32( ma1, 24 );			// sa per 32-bit lane
		ma1 = _mm256_packs_epi32( ma1, ma1 );		// 0 1 2 3 0 1 2 3
		ma1 = _mm256_unpacklo_epi16( ma1, ma1 );	// 0 0 1 1 2 2 3 3
		__m256i ma2 = ma1;
		ma1 = _mm256_unpacklo_epi16( ma1, ma1 );	// pixel 0,1
		ma2 = _mm256_unpackhi_epi16( ma2, ma2 );	// pixel 2,3
		ma1 = _mm256_sub_epi16( m255_, ma1 );		// 255 - sa
		ma2 = _mm256_sub_epi16( m255_, ma2 );

		__m256i md2 = d;
		d = _mm256_unpacklo_epi8( d, zero_ );
		d = _mm256_mullo_epi16( d, ma1 );			// d * (255 - sa)
		d = _mm256_srli_epi16( d, 8 );

		md2 = _mm256_unpackhi_epi8( md2, zero_ );
		md2 = _mm256_mullo_epi16( md2, ma2 );		// d * (255 - sa)
		md2 = _mm256_srli_epi16( md2, 8 );

		__m256i packed = _mm256_packus_epi16( d, md2 );
		packed = _mm256_adds_epu8( packed, s );		// + src (sat add per byte)
		return _mm256_or_si256( _mm256_and_si256( packed, colormask_ ),
		                        _mm256_and_si256( s_orig, alphamask_ ) );
	}
};
//--------------------------------------------------------------------
// di = di - di*a*opa + si*opa
//              ~~~~~Df ~~~~~~ Sf
//           ~~~~~~~~Ds
//      ~~~~~~~~~~~~~Dq
// additive alpha blend with opacity
// 詳細仕様は SSE2 側のコメント参照。
struct avx2_premul_alpha_blend_o_functor {
	const __m256i zero_;
	const __m256i opa_;
	const tjs_int opa_scalar_;
	const __m256i colormask_;
	const __m256i m255_;
	inline avx2_premul_alpha_blend_o_functor( tjs_int opa )
		: zero_( _mm256_setzero_si256() ), opa_(_mm256_set1_epi16((short)opa)),
		  opa_scalar_(opa),
		  colormask_(_mm256_set1_epi32(0x00ffffff)),
		  m255_(_mm256_set1_epi32(0xff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		const __m128i opa = _mm256_extracti128_si256( opa_, 0 );
		tjs_uint32 sa = s >> 24;
		tjs_uint32 sa_pmul = (sa * (tjs_uint32)opa_scalar_) >> 8;
		tjs_int32  sopa_inv = 255 - (tjs_int32)sa_pmul;

		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i mo_inv = _mm_cvtsi32_si128( sopa_inv );
		mo_inv = _mm_shufflelo_epi16( mo_inv, _MM_SHUFFLE( 0, 0, 0, 0 ) );

		ms = _mm_cvtepu8_epi16( ms );
		ms = _mm_mullo_epi16( ms, opa );		// s * opa per channel
		ms = _mm_srli_epi16( ms, 8 );			// s_pmul

		md = _mm_cvtepu8_epi16( md );
		md = _mm_mullo_epi16( md, mo_inv );		// d * (255 - sa_pmul)
		md = _mm_srli_epi16( md, 8 );

		md = _mm_add_epi16( md, ms );			// + s_pmul
		md = _mm_packus_epi16( md, md );
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (sa_pmul << 24);
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		// sa_pmul = (sa * opa) >> 8 per 32-bit lane
		__m256i sa32 = _mm256_srli_epi32( s, 24 );
		__m256i sa_pmul32 = _mm256_mullo_epi16( sa32, opa_ );
		sa_pmul32 = _mm256_srli_epi32( sa_pmul32, 8 );
		__m256i alpha_out = _mm256_slli_epi32( sa_pmul32, 24 );

		// (255 - sa_pmul) per lane → 4x16bit per pixel に broadcast
		__m256i sopa_inv32 = _mm256_sub_epi32( m255_, sa_pmul32 );
		__m256i ma  = _mm256_packs_epi32( sopa_inv32, sopa_inv32 );
		ma  = _mm256_unpacklo_epi16( ma, ma );
		__m256i ma2 = ma;
		ma  = _mm256_unpacklo_epi16( ma,  ma  );
		ma2 = _mm256_unpackhi_epi16( ma2, ma2 );

		// s_pmul = s * opa >> 8 (全チャネル)
		__m256i s_lo = _mm256_unpacklo_epi8( s, zero_ );
		s_lo = _mm256_mullo_epi16( s_lo, opa_ );
		s_lo = _mm256_srli_epi16( s_lo, 8 );
		__m256i s_hi = _mm256_unpackhi_epi8( s, zero_ );
		s_hi = _mm256_mullo_epi16( s_hi, opa_ );
		s_hi = _mm256_srli_epi16( s_hi, 8 );

		// d * (255 - sa_pmul) >> 8
		__m256i d_lo = _mm256_unpacklo_epi8( d, zero_ );
		d_lo = _mm256_mullo_epi16( d_lo, ma );
		d_lo = _mm256_srli_epi16( d_lo, 8 );
		__m256i d_hi = _mm256_unpackhi_epi8( d, zero_ );
		d_hi = _mm256_mullo_epi16( d_hi, ma2 );
		d_hi = _mm256_srli_epi16( d_hi, 8 );

		d_lo = _mm256_add_epi16( d_lo, s_lo );
		d_hi = _mm256_add_epi16( d_hi, s_hi );
		__m256i packed = _mm256_packus_epi16( d_lo, d_hi );

		return _mm256_or_si256( _mm256_and_si256( packed, colormask_ ), alpha_out );
	}
};
/*
	Di = sat(Si, (1-Sa)*Di)
	Da = Sa + Da - SaDa
*/
// additive alpha blend holding destination alpha
// C リファレンス:
//   result_rgb = sat_add( (d * (255-sa)) >> 8, s_rgb )
//   result_a   = dst.alpha            (HDA = Hold Destination Alpha)
struct avx2_premul_alpha_blend_hda_functor {
	const __m256i zero_;
	const __m256i unpack_colormask_;	// unpacked 16bit lanes 用 (alpha lane=0)
	const __m256i colormask_;
	const __m256i alphamask_packed_;
	const __m256i m255_;
	inline avx2_premul_alpha_blend_hda_functor() : zero_(_mm256_setzero_si256()),
		unpack_colormask_(_mm256_set_epi32(0x0000ffff,0xffffffff,0x0000ffff,0xffffffff,0x0000ffff,0xffffffff,0x0000ffff,0xffffffff)),
		colormask_(_mm256_set1_epi32(0x00FFFFFF)),
		alphamask_packed_(_mm256_set1_epi32(0xFF000000)),
		m255_(_mm256_set1_epi16(255)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		const __m128i unpack_colormask = _mm256_extracti128_si256( unpack_colormask_, 0 );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i mo = _mm_cvtsi32_si128( 255 - (tjs_int32)(s >> 24) );	// 255 - sa
		__m128i md = _mm_cvtsi32_si128( d );
		mo = _mm_shufflelo_epi16( mo, _MM_SHUFFLE( 0, 0, 0, 0 ) );
		ms = _mm_and_si128( ms, _mm_set1_epi32(0x00ffffff) );	// drop src alpha
		md = _mm_cvtepu8_epi16( md );
		md = _mm_mullo_epi16( md, mo );			// d * (255 - sa)
		md = _mm_srli_epi16( md, 8 );
		md = _mm_and_si128( md, unpack_colormask );	// alpha lane を 0 に
		md = _mm_packus_epi16( md, md );
		md = _mm_adds_epu8( md, ms );			// (d*(255-sa))>>8 + s.rgb
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (d & 0xff000000);
	}
	inline __m256i operator()( __m256i md, __m256i s ) const {
		__m256i d_orig = md;
		__m256i mo0 = s;
		mo0 = _mm256_srli_epi32( mo0, 24 );
		mo0 = _mm256_packs_epi32( mo0, mo0 );		// 0 1 2 3 0 1 2 3
		mo0 = _mm256_unpacklo_epi16( mo0, mo0 );	// 0 0 1 1 2 2 3 3
		__m256i mo1 = mo0;
		mo1 = _mm256_unpacklo_epi16( mo1, mo1 );	// pixel 0,1
		mo0 = _mm256_unpackhi_epi16( mo0, mo0 );	// pixel 2,3
		mo1 = _mm256_sub_epi16( m255_, mo1 );		// 255 - sa
		mo0 = _mm256_sub_epi16( m255_, mo0 );

		__m256i md0 = md;
		md  = _mm256_unpacklo_epi8( md, zero_ );	// d[1]
		md  = _mm256_mullo_epi16( md, mo1 );		// d[1] * (255-sa)
		md  = _mm256_srli_epi16( md, 8 );
		md  = _mm256_and_si256( md, unpack_colormask_ );

		md0 = _mm256_unpackhi_epi8( md0, zero_ );	// d[0]
		md0 = _mm256_mullo_epi16( md0, mo0 );		// d[0] * (255-sa)
		md0 = _mm256_srli_epi16( md0, 8 );
		md0 = _mm256_and_si256( md0, unpack_colormask_ );
		__m256i packed = _mm256_packus_epi16( md, md0 );

		s = _mm256_and_si256( s, colormask_ );		// drop src alpha
		packed = _mm256_adds_epu8( packed, s );		// (d*(255-sa))>>8 + s.rgb
		return _mm256_or_si256( _mm256_and_si256( packed, colormask_ ),
		                        _mm256_and_si256( d_orig, alphamask_packed_ ) );
	}
};
// additive alpha blend on additive alpha
// 詳細は SSE2 側のコメント参照。
struct avx2_premul_alpha_blend_a_functor {
	const __m256i zero_;
	const __m256i colormask_;
	const __m256i m255_;
	inline avx2_premul_alpha_blend_a_functor()
		: zero_(_mm256_setzero_si256()),
		  colormask_(_mm256_set1_epi32(0x00ffffff)),
		  m255_(_mm256_set1_epi32(0xff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 da = d >> 24;
		tjs_uint32 sa = s >> 24;
		tjs_uint32 new_a = da + sa - ((da * sa) >> 8);
		new_a -= (new_a >> 8);
		tjs_int32  sopa_inv = 255 - (tjs_int32)sa;

		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i mo = _mm_cvtsi32_si128( sopa_inv );
		mo = _mm_shufflelo_epi16( mo, _MM_SHUFFLE( 0, 0, 0, 0 ) );
		md = _mm_cvtepu8_epi16( md );
		md = _mm_mullo_epi16( md, mo );			// d * (255 - sa)
		md = _mm_srli_epi16( md, 8 );
		md = _mm_packus_epi16( md, md );
		md = _mm_adds_epu8( md, ms );			// + s (sat add)
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (new_a << 24);
	}
	inline __m256i operator()( __m256i d, __m256i s ) const {
		// alpha 計算: da + sa - (da*sa>>8) を 32bit lane 単位
		__m256i sa32 = _mm256_srli_epi32( s, 24 );
		__m256i da32 = _mm256_srli_epi32( d, 24 );
		__m256i prod = _mm256_mullo_epi16( sa32, da32 );
		prod = _mm256_srli_epi32( prod, 8 );
		__m256i new_a = _mm256_add_epi32( sa32, da32 );
		new_a = _mm256_sub_epi32( new_a, prod );
		__m256i clamp = _mm256_srli_epi32( new_a, 8 );
		new_a = _mm256_sub_epi32( new_a, clamp );
		__m256i alpha_out = _mm256_slli_epi32( new_a, 24 );

		// (255 - sa) per lane → 4x16bit per pixel に broadcast
		__m256i sopa_inv32 = _mm256_sub_epi32( m255_, sa32 );
		__m256i ma  = _mm256_packs_epi32( sopa_inv32, sopa_inv32 );
		ma  = _mm256_unpacklo_epi16( ma, ma );
		__m256i ma2 = ma;
		ma  = _mm256_unpacklo_epi16( ma,  ma  );
		ma2 = _mm256_unpackhi_epi16( ma2, ma2 );

		__m256i d_lo = _mm256_unpacklo_epi8( d, zero_ );
		d_lo = _mm256_mullo_epi16( d_lo, ma );
		d_lo = _mm256_srli_epi16( d_lo, 8 );
		__m256i d_hi = _mm256_unpackhi_epi8( d, zero_ );
		d_hi = _mm256_mullo_epi16( d_hi, ma2 );
		d_hi = _mm256_srli_epi16( d_hi, 8 );

		__m256i packed = _mm256_packus_epi16( d_lo, d_hi );
		packed = _mm256_adds_epu8( packed, s );		// + s (sat add per byte)
		return _mm256_or_si256( _mm256_and_si256( packed, colormask_ ), alpha_out );
	}
};

// opacity値を使う
// 非 HDA バリアント: 結果のアルファは 0 (C リファレンス互換)
struct avx2_const_alpha_blend_functor {
	const __m256i opa_;
	const __m256i zero_;
	const __m256i colormask_;
	inline avx2_const_alpha_blend_functor( tjs_int32 opa )
		: zero_(_mm256_setzero_si256()),
		  opa_(_mm256_set1_epi16((short)opa)),
		  colormask_(_mm256_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		const __m128i opa = _mm256_extracti128_si256( opa_, 0 );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		ms = _mm_cvtepu8_epi16( ms );		// 00 ss 00 ss 00 ss 00 ss
		md = _mm_cvtepu8_epi16( md );		// 00 dd 00 dd 00 dd 00 dd
		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, opa );	// ms *= ma
		ms = _mm_srli_epi16( ms, 8 );		// ms >>= 8
		md = _mm_add_epi8( md, ms );		// md += ms : d + ((s-d)*sopa)>>8
		md = _mm_packus_epi16( md, md );	// pack
		return _mm_cvtsi128_si32( md ) & 0x00ffffff;
	}
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const {
		__m256i ms2 = ms1;
		__m256i md2 = md1;

		ms1 = _mm256_unpacklo_epi8( ms1, zero_ );
		md1 = _mm256_unpacklo_epi8( md1, zero_ );
		ms1 = _mm256_sub_epi16( ms1, md1 );	// s -= d
		ms1 = _mm256_mullo_epi16( ms1, opa_ );	// s *= a
		ms1 = _mm256_srli_epi16( ms1, 8 );		// s >>= 8
		md1 = _mm256_add_epi8( md1, ms1 );		// d += s

		ms2 = _mm256_unpackhi_epi8( ms2, zero_ );
		md2 = _mm256_unpackhi_epi8( md2, zero_ );
		ms2 = _mm256_sub_epi16( ms2, md2 );		// s -= d
		ms2 = _mm256_mullo_epi16( ms2, opa_ );		// s *= a
		ms2 = _mm256_srli_epi16( ms2, 8 );			// s >>= 8
		md2 = _mm256_add_epi8( md2, ms2 );		// d += s
		return _mm256_and_si256( _mm256_packus_epi16( md1, md2 ), colormask_ );
	}
};
typedef avx2_variation_hda<avx2_const_alpha_blend_functor>	avx2_const_alpha_blend_hda_functor;

// テーブルを使わないように実装したので、要テスト
struct avx2_const_alpha_blend_d_functor {
	const tjs_int32 opa32_;
	const __m256i opa_;
	const __m256i m255_;
	const __m256i zero_;
	const __m256i colormask_;
	const __m256 m65535_;
	inline avx2_const_alpha_blend_d_functor( tjs_int32 opa ) : m255_(_mm256_set1_epi32(255)), m65535_(_mm256_set1_ps(65535.0f)),
		zero_(_mm256_setzero_si256()), colormask_(_mm256_set1_epi32(0x00ffffff)),
		opa32_(opa<<8), opa_(_mm256_set1_epi16((short)opa)) {}

	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 addr = opa32_ + (d>>24);
		tjs_uint32 sopa = TVPOpacityOnOpacityTable[addr];
		__m128i ma = _mm_cvtsi32_si128( sopa );
		ma = _mm_shufflelo_epi16( ma, _MM_SHUFFLE( 0, 0, 0, 0 )  );	// 0000000000000000 00oo00oo00oo00oo
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		ms = _mm_cvtepu8_epi16( ms );		// 00 ss 00 ss 00 ss 00 ss
		md = _mm_cvtepu8_epi16( md );		// 00 dd 00 dd 00 dd 00 dd
		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, ma );		// ms *= ma
		ms = _mm_srli_epi16( ms, 8 );		// ms >>= 8
		md = _mm_add_epi8( md, ms );		// md += ms : d + ((s-d)*sopa)>>8
		md = _mm_packus_epi16( md, md );	// pack
		tjs_uint32 ret = _mm_cvtsi128_si32( md );		// store
		addr = TVPNegativeMulTable[addr] << 24;
		return (ret&0x00ffffff) | addr;
	}
	// rcp_ps 近似ではなく C リファレンスと同じテーブル直接参照に変更
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const {
		// addr = (opa<<8) + (d>>24) per 32-bit lane
		__m256i da32 = _mm256_srli_epi32( md1, 24 );
		__m256i opa32 = _mm256_set1_epi32( opa32_ );	// opa<<8 を 32bit lane へ
		__m256i maddr = _mm256_add_epi32( opa32, da32 );

		__m256i ma = _mm256_set_epi32(
			TVPOpacityOnOpacityTable[M256I_U32(maddr,7)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,6)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,5)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,4)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,3)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,2)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,1)],
			TVPOpacityOnOpacityTable[M256I_U32(maddr,0)] );
		ma = _mm256_packs_epi32( ma, ma );
		ma = _mm256_unpacklo_epi16( ma, ma );
		__m256i ma2 = ma;
		ma  = _mm256_unpacklo_epi16( ma,  ma  );
		ma2 = _mm256_unpackhi_epi16( ma2, ma2 );

		__m256i ms_lo = _mm256_unpacklo_epi8( ms1, zero_ );
		__m256i md_lo = _mm256_unpacklo_epi8( md1, zero_ );
		ms_lo = _mm256_sub_epi16( ms_lo, md_lo );
		ms_lo = _mm256_mullo_epi16( ms_lo, ma );
		md_lo = _mm256_slli_epi16( md_lo, 8 );
		md_lo = _mm256_add_epi16( md_lo, ms_lo );
		md_lo = _mm256_srli_epi16( md_lo, 8 );

		__m256i ms_hi = _mm256_unpackhi_epi8( ms1, zero_ );
		__m256i md_hi = _mm256_unpackhi_epi8( md1, zero_ );
		ms_hi = _mm256_sub_epi16( ms_hi, md_hi );
		ms_hi = _mm256_mullo_epi16( ms_hi, ma2 );
		md_hi = _mm256_slli_epi16( md_hi, 8 );
		md_hi = _mm256_add_epi16( md_hi, ms_hi );
		md_hi = _mm256_srli_epi16( md_hi, 8 );

		__m256i packed = _mm256_packus_epi16( md_lo, md_hi );

		__m256i mneg = _mm256_set_epi32(
			TVPNegativeMulTable[M256I_U32(maddr,7)],
			TVPNegativeMulTable[M256I_U32(maddr,6)],
			TVPNegativeMulTable[M256I_U32(maddr,5)],
			TVPNegativeMulTable[M256I_U32(maddr,4)],
			TVPNegativeMulTable[M256I_U32(maddr,3)],
			TVPNegativeMulTable[M256I_U32(maddr,2)],
			TVPNegativeMulTable[M256I_U32(maddr,1)],
			TVPNegativeMulTable[M256I_U32(maddr,0)] );
		mneg = _mm256_slli_epi32( mneg, 24 );

		packed = _mm256_and_si256( packed, colormask_ );
		return _mm256_or_si256( packed, mneg );
	}
};

struct avx2_const_alpha_blend_a_functor {
	const tjs_uint32 opa32_;
	const __m256i opa_;
	const __m256i colormask_;
	const struct avx2_premul_alpha_blend_a_functor blend_;
	inline avx2_const_alpha_blend_a_functor( tjs_int32 opa )
		: colormask_(_mm256_set1_epi32(0x00ffffff)), opa32_(opa<<24), opa_(_mm256_set1_epi32(opa<<24)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		return blend_( d, (s&0x00ffffff)|opa32_ );
	}
	inline __m256i operator()( __m256i md1, __m256i ms1 ) const {
		ms1 = _mm256_and_si256( ms1, colormask_ );
		ms1 = _mm256_or_si256( ms1, opa_ );
		return blend_( md1, ms1 );
	}
};

//																	avx2_const_alpha_blend_functor;
typedef avx2_const_alpha_blend_functor								avx2_const_alpha_blend_sd_functor;
// tjs_uint32 avx2_const_alpha_blend_functor::operator()( tjs_uint32 d, tjs_uint32 s )
// tjs_uint32 avx2_const_alpha_blend_sd_functor::operator()( tjs_uint32 s1, tjs_uint32 s2 )
// と引数は異なるが、処理内容は同じ
// const_alpha_blend は、dest と src1 を共有しているようなもの dest = dest * src
// const_alpha_blend_sd は、dest = src1 * src2

// avx2_const_copy_functor = TVPCopy はない、memcpy になってる
// avx2_color_copy_functor = TVPCopyColor / TVPLinTransColorCopy
// avx2_alpha_copy_functor = TVPCopyMask
// avx2_color_opaque_functor = TVPCopyOpaqueImage
// avx2_const_alpha_blend_functor = TVPConstAlphaBlend
// avx2_const_alpha_blend_hda_functor = TVPConstAlphaBlend_HDA
// avx2_const_alpha_blend_d_functor = TVPConstAlphaBlend_a
// avx2_const_alpha_blend_a_functor = TVPConstAlphaBlend_a

//--------------------------------------------------------------------
// ここまでアルファブレンド
// 加算合成などはps以外はAVX2では対応しない
//--------------------------------------------------------------------

#endif // __BLEND_FUNCTOR_AVX2_H__
