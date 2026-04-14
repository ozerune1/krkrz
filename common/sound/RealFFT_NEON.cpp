//---------------------------------------------------------------------------
/*
	Risa [りさ]		 alias 吉里吉里3 [kirikiri-3]
	 stands for "Risa Is a Stagecraft Architecture"
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
//! @file
//! @brief 実数離散フーリエ変換 (ARM NEON 版)
//---------------------------------------------------------------------------

// RealFFT_SSE.cpp の NEON 版。#define で _mm_* → NEON の薄いラッパを定義し
// ロジック本体は SSE 版をほぼそのままコピーしている。PCS_* 定数・PFV_0P5 は
// xmmlib.cpp の memory 表現を局所 const として再定義した。

#include "tjsCommHead.h"
#include "xmmlib.h"

#if defined(TVP_SOUND_HAS_ARM_SIMD)

#include <arm_neon.h>

//---------------------------------------------------------------------------
// SSE 互換 helper (NEON 実装)
//---------------------------------------------------------------------------

#define __m128 float32x4_t
// __m64 は SSE/MMX の 64bit 型。ARM にはないので、コピー元の (__m64*)(ptr)
// キャストを素通しさせるために void に alias する。
#define __m64 void

// 算術
#define _mm_add_ps(a, b)  vaddq_f32((a), (b))
#define _mm_sub_ps(a, b)  vsubq_f32((a), (b))
#define _mm_mul_ps(a, b)  vmulq_f32((a), (b))
#define _mm_set1_ps(x)    vdupq_n_f32(x)

// aligned load/store
#define _mm_load_ps(p)    vld1q_f32((const float*)(p))
#define _mm_store_ps(p, x) vst1q_f32((float*)(p), (x))

// PM128: SSE 版では *(__m128*)ptr キャスト。NEON 側は 4-float をロードするだけ。
#undef PM128
#define PM128(x) vld1q_f32((const float*)(x))

// 64-bit 半分 load/store
static inline float32x4_t _tp_loadl_pi(float32x4_t x, const void* p) {
	return vcombine_f32(vld1_f32((const float*)p), vget_high_f32(x));
}
static inline float32x4_t _tp_loadh_pi(float32x4_t x, const void* p) {
	return vcombine_f32(vget_low_f32(x), vld1_f32((const float*)p));
}
static inline void _tp_storel_pi(void* p, float32x4_t x) {
	vst1_f32((float*)p, vget_low_f32(x));
}
static inline void _tp_storeh_pi(void* p, float32x4_t x) {
	vst1_f32((float*)p, vget_high_f32(x));
}
#define _mm_loadl_pi(x, p)  _tp_loadl_pi((x), (const void*)(p))
#define _mm_loadh_pi(x, p)  _tp_loadh_pi((x), (const void*)(p))
#define _mm_storel_pi(p, x) _tp_storel_pi((void*)(p), (x))
#define _mm_storeh_pi(p, x) _tp_storeh_pi((void*)(p), (x))

// movelh/movehl
static inline float32x4_t _tp_movelh_ps(float32x4_t a, float32x4_t b) {
	// (a[0], a[1], b[0], b[1])
	return vcombine_f32(vget_low_f32(a), vget_low_f32(b));
}
static inline float32x4_t _tp_movehl_ps(float32x4_t a, float32x4_t b) {
	// (b[2], b[3], a[2], a[3]) — b が先 (SSE 仕様)
	return vcombine_f32(vget_high_f32(b), vget_high_f32(a));
}
#define _mm_movelh_ps(a, b) _tp_movelh_ps((a), (b))
#define _mm_movehl_ps(a, b) _tp_movehl_ps((a), (b))

// スカラ load/sub
static inline float32x4_t _tp_load_ss(const float* p) {
	// (*p, 0, 0, 0)
	return vsetq_lane_f32(*p, vdupq_n_f32(0.0f), 0);
}
static inline float32x4_t _tp_sub_ss(float32x4_t a, float32x4_t b) {
	// lane0 だけ減算、lanes 1-3 は a のまま
	float v = vgetq_lane_f32(a, 0) - vgetq_lane_f32(b, 0);
	return vsetq_lane_f32(v, a, 0);
}
#define _mm_load_ss(p)   _tp_load_ss((const float*)(p))
#define _mm_sub_ss(a, b) _tp_sub_ss((a), (b))

// XOR (bitwise)
static inline float32x4_t _tp_xor_ps(float32x4_t x, float32x4_t y) {
	return vreinterpretq_f32_u32(
		veorq_u32(vreinterpretq_u32_f32(x), vreinterpretq_u32_f32(y)));
}
#define _mm_xor_ps(a, b) _tp_xor_ps((a), (b))

// MMX 後始末 (NEON では空)
#define _mm_empty() ((void)0)

// _MM_SHUFFLE: z,y,x,w の 4bit 組
#undef _MM_SHUFFLE
#define _MM_SHUFFLE(z, y, x, w) (((z)<<6) | ((y)<<4) | ((x)<<2) | (w))

// _mm_shuffle_ps(a, b, mask):
//   result[0] = a[mask & 3]
//   result[1] = a[(mask>>2) & 3]
//   result[2] = b[(mask>>4) & 3]
//   result[3] = b[(mask>>6) & 3]
template<int W, int X, int Y, int Z>
static inline float32x4_t _tp_shuf_ps(float32x4_t a, float32x4_t b) {
	float32x4_t r = vdupq_n_f32(0.0f);
	r = vsetq_lane_f32(vgetq_lane_f32(a, W), r, 0);
	r = vsetq_lane_f32(vgetq_lane_f32(a, X), r, 1);
	r = vsetq_lane_f32(vgetq_lane_f32(b, Y), r, 2);
	r = vsetq_lane_f32(vgetq_lane_f32(b, Z), r, 3);
	return r;
}
#define _mm_shuffle_ps(a, b, mask) \
	_tp_shuf_ps<((mask)&3), (((mask)>>2)&3), (((mask)>>4)&3), (((mask)>>6)&3)>((a), (b))

// PCS_* / PFV_0P5: xmmlib.cpp と同じ memory 表現で local に定義。
// _SALIGN16 は NEON 側では不要 (unaligned load 可)。
// xmmlib.cpp のエントリ順に合わせて uint32_t[4] で定義している。
alignas(16) static const uint32_t _PCS_RRNN[4] = {0x00000000u, 0x00000000u, 0x80000000u, 0x80000000u};
alignas(16) static const uint32_t _PCS_RNNR[4] = {0x80000000u, 0x00000000u, 0x00000000u, 0x80000000u};
alignas(16) static const uint32_t _PCS_NRNR[4] = {0x80000000u, 0x00000000u, 0x80000000u, 0x00000000u};
alignas(16) static const uint32_t _PCS_NNNR[4] = {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u};
alignas(16) static const uint32_t _PCS_RNRN[4] = {0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u};
alignas(16) static const uint32_t _PCS_NRRN[4] = {0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u};
alignas(16) static const uint32_t _PCS_NNRN[4] = {0x00000000u, 0x80000000u, 0x00000000u, 0x00000000u};
alignas(16) static const float    _PFV_0P5[4] = {0.5f, 0.5f, 0.5f, 0.5f};

// PCS_* / PFV_0P5 を local 名に shadow する
#undef PCS_RRNN
#undef PCS_RNNR
#undef PCS_NRNR
#undef PCS_NNNR
#undef PCS_RNRN
#undef PCS_NRRN
#undef PCS_NNRN
#undef PFV_0P5
#define PCS_RRNN _PCS_RRNN
#define PCS_RNNR _PCS_RNNR
#define PCS_NRNR _PCS_NRNR
#define PCS_NNNR _PCS_NNNR
#define PCS_RNRN _PCS_RNRN
#define PCS_NRRN _PCS_NRRN
#define PCS_NNRN _PCS_NNRN
#define PFV_0P5  _PFV_0P5

//---------------------------------------------------------------------------
// 以下 RealFFT_SSE.cpp の本体をほぼそのままコピー (GNUC 側のみ、rdft_sse →
// rdft_neon)。ロジック差分なし。
//---------------------------------------------------------------------------

static inline void cft1st(int n, float * __restrict a, float * __restrict w)
{
	int		j, k1, k2;

	__m128	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5;
	XMM0	 = _mm_loadl_pi(XMM0, (__m64*)(a   ));
	XMM2	 = _mm_loadl_pi(XMM2, (__m64*)(a+ 2));
	XMM0	 = _mm_loadh_pi(XMM0, (__m64*)(a+ 4));
	XMM2	 = _mm_loadh_pi(XMM2, (__m64*)(a+ 6));
	XMM1	 = XMM0;
	XMM0	 = _mm_add_ps(XMM0, XMM2);
	XMM1	 = _mm_sub_ps(XMM1, XMM2);
	XMM2	 = XMM0;
	XMM3	 = XMM1;
	XMM0	 = _mm_movelh_ps(XMM0, XMM0);
	XMM2	 = _mm_movehl_ps(XMM2, XMM2);
	XMM1	 = _mm_movelh_ps(XMM1, XMM1);
	XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(2,3,2,3));
	XMM2	 = _mm_xor_ps(XMM2, PM128(PCS_RRNN));
	XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_RNNR));
	XMM0	 = _mm_add_ps(XMM0, XMM2);
	XMM1	 = _mm_add_ps(XMM1, XMM3);
	_mm_storel_pi((__m64*)(a   ), XMM0);
	_mm_storeh_pi((__m64*)(a+ 4), XMM0);
	_mm_storel_pi((__m64*)(a+ 2), XMM1);
	_mm_storeh_pi((__m64*)(a+ 6), XMM1);
	XMM0	 = _mm_loadl_pi(XMM0, (__m64*)(a+ 8));
	XMM2	 = _mm_loadl_pi(XMM2, (__m64*)(a+10));
	XMM0	 = _mm_loadh_pi(XMM0, (__m64*)(a+12));
	XMM2	 = _mm_loadh_pi(XMM2, (__m64*)(a+14));
	XMM1	 = XMM0;
	XMM0	 = _mm_add_ps(XMM0, XMM2);
	XMM1	 = _mm_sub_ps(XMM1, XMM2);
	XMM2	 = XMM0;
	XMM3	 = XMM1;
	XMM0	 = _mm_shuffle_ps(XMM0, XMM0, _MM_SHUFFLE(0,3,1,0));
	XMM2	 = _mm_shuffle_ps(XMM2, XMM2, _MM_SHUFFLE(2,1,3,2));
	XMM1	 = _mm_shuffle_ps(XMM1, XMM1, _MM_SHUFFLE(2,3,1,0));
	XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(1,0,2,3));
	XMM2	 = _mm_xor_ps(XMM2, PM128(PCS_RRNN));
	XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_RNNR));
	XMM0	 = _mm_add_ps(XMM0, XMM2);
	XMM1	 = _mm_add_ps(XMM1, XMM3);
	_mm_storel_pi((__m64*)(a+ 8), XMM0);
	_mm_storeh_pi((__m64*)(a+12), XMM0);
	XMM2	 = XMM1;
	XMM3	 = _mm_load_ss(w+2);
	XMM1	 = _mm_shuffle_ps(XMM1, XMM1, _MM_SHUFFLE(3,3,0,0));
	XMM2	 = _mm_shuffle_ps(XMM2, XMM2, _MM_SHUFFLE(2,2,1,1));
	XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(0,0,0,0));
	XMM2	 = _mm_xor_ps(XMM2, PM128(PCS_NRNR));
	XMM1	 = _mm_add_ps(XMM1, XMM2);
	XMM1	 = _mm_mul_ps(XMM1, XMM3);
	_mm_storel_pi((__m64*)(a+10), XMM1);
	_mm_storeh_pi((__m64*)(a+14), XMM1);
	k1 = 0;
	for (j = 16; j < n; j += 16) {
		k1		+= 2;
		k2		 = 2 * k1;
		XMM4	 = _mm_loadh_pi(XMM4, (__m64*)(w+k1	 ));
		XMM5	 = _mm_loadl_pi(XMM5, (__m64*)(w+k2	 ));
		XMM0	 = XMM5;
		XMM1	 = XMM4;
		XMM0	 = _mm_shuffle_ps(XMM0, XMM0, _MM_SHUFFLE(0,1,0,1));
		XMM1	 = _mm_shuffle_ps(XMM1, XMM1, _MM_SHUFFLE(3,3,3,3));
		XMM0	 = _mm_mul_ps(XMM0, XMM1);
		XMM0	 = _mm_add_ps(XMM0, XMM0);
		XMM0	 = _mm_sub_ps(XMM0, XMM5);
		XMM0	 = _mm_xor_ps(XMM0, PM128(PCS_NRNR));
		XMM5	 = _mm_movelh_ps(XMM5, XMM0);

		XMM0	 = _mm_loadl_pi(XMM0, (__m64*)(a+j	 ));
		XMM2	 = _mm_loadl_pi(XMM2, (__m64*)(a+j+ 2));
		XMM0	 = _mm_loadh_pi(XMM0, (__m64*)(a+j+ 4));
		XMM2	 = _mm_loadh_pi(XMM2, (__m64*)(a+j+ 6));
		XMM1	 = XMM0;
		XMM0	 = _mm_add_ps(XMM0, XMM2);
		XMM1	 = _mm_sub_ps(XMM1, XMM2);
		XMM2	 = XMM0;
		XMM3	 = XMM1;
		XMM0	 = _mm_movelh_ps(XMM0, XMM0);
		XMM2	 = _mm_movehl_ps(XMM2, XMM2);
		XMM1	 = _mm_movelh_ps(XMM1, XMM1);
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(2,3,2,3));
		XMM2	 = _mm_xor_ps(XMM2, PM128(PCS_RRNN));
		XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_RNNR));
		XMM0	 = _mm_add_ps(XMM0, XMM2);
		XMM1	 = _mm_add_ps(XMM1, XMM3);
		_mm_storel_pi((__m64*)(a+j	 ), XMM0);
		XMM2	 = XMM0;
		XMM3	 = XMM4;
		XMM2	 = _mm_shuffle_ps(XMM2, XMM2, _MM_SHUFFLE(2,3,2,3));
		XMM4	 = _mm_shuffle_ps(XMM4, XMM4, _MM_SHUFFLE(2,2,2,2));
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(3,3,3,3));
		XMM0	 = _mm_mul_ps(XMM0, XMM4);
		XMM2	 = _mm_mul_ps(XMM2, XMM3);
		XMM2	 = _mm_xor_ps(XMM2, PM128(PCS_NRNR));
		XMM0	 = _mm_add_ps(XMM0, XMM2);
		_mm_storeh_pi((__m64*)(a+j+ 4), XMM0);
		XMM4	 = XMM1;
		XMM3	 = XMM5;
		XMM4	 = _mm_shuffle_ps(XMM4, XMM4, _MM_SHUFFLE(2,3,0,1));
		XMM5	 = _mm_shuffle_ps(XMM5, XMM5, _MM_SHUFFLE(2,2,0,0));
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(3,3,1,1));
		XMM1	 = _mm_mul_ps(XMM1, XMM5);
		XMM4	 = _mm_mul_ps(XMM4, XMM3);
		XMM4	 = _mm_xor_ps(XMM4, PM128(PCS_NRNR));
		XMM1	 = _mm_add_ps(XMM1, XMM4);
		_mm_storel_pi((__m64*)(a+j+ 2), XMM1);
		_mm_storeh_pi((__m64*)(a+j+ 6), XMM1);
		XMM4	 = _mm_loadh_pi(XMM4, (__m64*)(w+k1	 ));
		XMM5	 = _mm_loadl_pi(XMM5, (__m64*)(w+k2+2));
		XMM0	 = XMM5;
		XMM1	 = XMM4;
		XMM0	 = _mm_shuffle_ps(XMM0, XMM0, _MM_SHUFFLE(0,1,0,1));
		XMM1	 = _mm_shuffle_ps(XMM1, XMM1, _MM_SHUFFLE(2,2,2,2));
		XMM0	 = _mm_mul_ps(XMM0, XMM1);
		XMM0	 = _mm_add_ps(XMM0, XMM0);
		XMM0	 = _mm_sub_ps(XMM0, XMM5);
		XMM0	 = _mm_xor_ps(XMM0, PM128(PCS_NRNR));
		XMM5	 = _mm_movelh_ps(XMM5, XMM0);

		XMM0	 = _mm_loadl_pi(XMM0, (__m64*)(a+j+ 8));
		XMM2	 = _mm_loadl_pi(XMM2, (__m64*)(a+j+10));
		XMM0	 = _mm_loadh_pi(XMM0, (__m64*)(a+j+12));
		XMM2	 = _mm_loadh_pi(XMM2, (__m64*)(a+j+14));
		XMM1	 = XMM0;
		XMM0	 = _mm_add_ps(XMM0, XMM2);
		XMM1	 = _mm_sub_ps(XMM1, XMM2);
		XMM2	 = XMM0;
		XMM3	 = XMM1;
		XMM0	 = _mm_movelh_ps(XMM0, XMM0);
		XMM2	 = _mm_movehl_ps(XMM2, XMM2);
		XMM1	 = _mm_movelh_ps(XMM1, XMM1);
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(2,3,2,3));
		XMM2	 = _mm_xor_ps(XMM2, PM128(PCS_RRNN));
		XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_RNNR));
		XMM0	 = _mm_add_ps(XMM0, XMM2);
		XMM1	 = _mm_add_ps(XMM1, XMM3);
		_mm_storel_pi((__m64*)(a+j+ 8), XMM0);
		XMM2	 = XMM0;
		XMM3	 = XMM4;
		XMM0	 = _mm_shuffle_ps(XMM0, XMM0, _MM_SHUFFLE(2,3,2,3));
		XMM4	 = _mm_shuffle_ps(XMM4, XMM4, _MM_SHUFFLE(2,2,2,2));
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(3,3,3,3));
		XMM0	 = _mm_mul_ps(XMM0, XMM4);
		XMM2	 = _mm_mul_ps(XMM2, XMM3);
		XMM0	 = _mm_xor_ps(XMM0, PM128(PCS_NRNR));
		XMM0	 = _mm_sub_ps(XMM0, XMM2);
		_mm_storeh_pi((__m64*)(a+j+12), XMM0);
		XMM4	 = XMM1;
		XMM3	 = XMM5;
		XMM4	 = _mm_shuffle_ps(XMM4, XMM4, _MM_SHUFFLE(2,3,0,1));
		XMM5	 = _mm_shuffle_ps(XMM5, XMM5, _MM_SHUFFLE(2,2,0,0));
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(3,3,1,1));
		XMM1	 = _mm_mul_ps(XMM1, XMM5);
		XMM4	 = _mm_mul_ps(XMM4, XMM3);
		XMM4	 = _mm_xor_ps(XMM4, PM128(PCS_NRNR));
		XMM1	 = _mm_add_ps(XMM1, XMM4);
		_mm_storel_pi((__m64*)(a+j+10), XMM1);
		_mm_storeh_pi((__m64*)(a+j+14), XMM1);
	}
}


static inline void cftmdl(int n, int l, float * __restrict a, float * __restrict w)
{
	int j, j1, j2, j3, k, k1, k2, m, m2;
	float wk1r, wk1i, wk2r, wk2i, wk3r, wk3i;
	__m128	XMM6;
	__m128	pwk1r, pwk1i, pwk2r, pwk2i, pwk3r, pwk3i;

	m = l << 2;
	for (j = 0; j < l; j += 4) {
		__m128	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5;

		j1 = j	+ l;
		j2 = j1 + l;
		j3 = j2 + l;

		XMM0	 = _mm_load_ps(a+j );
		XMM2	 = _mm_load_ps(a+j2);
		XMM1	 = XMM0;
		XMM3	 = XMM2;
		XMM0	 = _mm_add_ps(XMM0, PM128(a+j1));
		XMM1	 = _mm_sub_ps(XMM1, PM128(a+j1));
		XMM2	 = _mm_add_ps(XMM2, PM128(a+j3));
		XMM3	 = _mm_sub_ps(XMM3, PM128(a+j3));
		XMM4	 = XMM0;
		XMM5	 = XMM1;
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(2,3,0,1));
		XMM0	 = _mm_add_ps(XMM0, XMM2);
		XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_NRNR));
		XMM4	 = _mm_sub_ps(XMM4, XMM2);
		XMM1	 = _mm_add_ps(XMM1, XMM3);
		XMM5	 = _mm_sub_ps(XMM5, XMM3);
		_mm_store_ps(a+j , XMM0);
		_mm_store_ps(a+j1, XMM1);
		_mm_store_ps(a+j2, XMM4);
		_mm_store_ps(a+j3, XMM5);
	}
	XMM6	 = _mm_set1_ps(w[2]);
	for (j = m; j < l + m; j += 4) {
		__m128	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5;

		j1 = j	+ l;
		j2 = j1 + l;
		j3 = j2 + l;

		XMM0	 = _mm_load_ps(a+j );
		XMM2	 = _mm_load_ps(a+j2);
		XMM1	 = XMM0;
		XMM3	 = XMM2;
		XMM0	 = _mm_add_ps(XMM0, PM128(a+j1));
		XMM1	 = _mm_sub_ps(XMM1, PM128(a+j1));
		XMM2	 = _mm_add_ps(XMM2, PM128(a+j3));
		XMM3	 = _mm_sub_ps(XMM3, PM128(a+j3));

		XMM4	 = XMM0;
		XMM5	 = XMM0;
		XMM4	 = _mm_shuffle_ps(XMM4, XMM2, _MM_SHUFFLE(3,1,2,0));
		XMM5	 = _mm_shuffle_ps(XMM5, XMM2, _MM_SHUFFLE(2,0,3,1));
		XMM4	 = _mm_shuffle_ps(XMM4, XMM4, _MM_SHUFFLE(1,3,0,2));
		XMM5	 = _mm_shuffle_ps(XMM5, XMM5, _MM_SHUFFLE(3,1,2,0));
		XMM0	 = _mm_add_ps(XMM0, XMM2);
		XMM4	 = _mm_sub_ps(XMM4, XMM5);
		_mm_store_ps(a+j , XMM0);
		_mm_store_ps(a+j2, XMM4);

		XMM0	 = XMM1;
		XMM2	 = XMM3;
		XMM0	 = _mm_shuffle_ps(XMM0, XMM0, _MM_SHUFFLE(2,2,0,0));
		XMM1	 = _mm_shuffle_ps(XMM1, XMM1, _MM_SHUFFLE(3,3,1,1));
		XMM2	 = _mm_shuffle_ps(XMM2, XMM2, _MM_SHUFFLE(2,2,0,0));
		XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(3,3,1,1));
		XMM4	 = XMM0;
		XMM5	 = XMM1;
		XMM0	 = _mm_sub_ps(XMM0, XMM3);
		XMM1	 = _mm_add_ps(XMM1, XMM2);
		XMM2	 = _mm_sub_ps(XMM2, XMM5);
		XMM3	 = _mm_add_ps(XMM3, XMM4);
		XMM1	 = _mm_xor_ps(XMM1, PM128(PCS_NRNR));
		XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_NRNR));
		XMM0	 = _mm_add_ps(XMM0, XMM1);
		XMM2	 = _mm_add_ps(XMM2, XMM3);
		XMM0	 = _mm_mul_ps(XMM0, XMM6);
		XMM2	 = _mm_mul_ps(XMM2, XMM6);
		_mm_store_ps(a+j1, XMM0);
		_mm_store_ps(a+j3, XMM2);
	}
	k1 = 0;
	m2 = 2 * m;
	for (k = m2; k < n; k += m2) {
		k1 += 2;
		k2 = 2 * k1;
		wk2r = w[k1];
		wk2i = w[k1 + 1];
		wk1r = w[k2];
		wk1i = w[k2 + 1];
		wk3r = wk1r - 2 * wk2i * wk1i;
		wk3i = 2 * wk2i * wk1r - wk1i;
		pwk1r	 = _mm_set1_ps(wk1r);
		pwk1i	 = _mm_set1_ps(wk1i);
		pwk2r	 = _mm_set1_ps(wk2r);
		pwk2i	 = _mm_set1_ps(wk2i);
		pwk3r	 = _mm_set1_ps(wk3r);
		pwk3i	 = _mm_set1_ps(wk3i);
		pwk1i	 = _mm_xor_ps(pwk1i, PM128(PCS_NRNR));
		pwk2i	 = _mm_xor_ps(pwk2i, PM128(PCS_NRNR));
		pwk3i	 = _mm_xor_ps(pwk3i, PM128(PCS_NRNR));
		for (j = k; j < l + k; j += 4) {
			__m128	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5;

			j1 = j	+ l;
			j2 = j1 + l;
			j3 = j2 + l;
			XMM0	 = _mm_load_ps(a+j );
			XMM2	 = _mm_load_ps(a+j2);
			XMM1	 = XMM0;
			XMM3	 = XMM2;
			XMM0	 = _mm_add_ps(XMM0, PM128(a+j1));
			XMM1	 = _mm_sub_ps(XMM1, PM128(a+j1));
			XMM2	 = _mm_add_ps(XMM2, PM128(a+j3));
			XMM3	 = _mm_sub_ps(XMM3, PM128(a+j3));

			XMM4	 = XMM0;
			XMM5	 = XMM0;
			XMM6	 = XMM2;
			XMM5	 = _mm_shuffle_ps(XMM5, XMM5, _MM_SHUFFLE(2,3,0,1));
			XMM6	 = _mm_shuffle_ps(XMM6, XMM6, _MM_SHUFFLE(2,3,0,1));
			XMM4	 = _mm_sub_ps(XMM4, XMM2);
			XMM5	 = _mm_sub_ps(XMM5, XMM6);
			XMM4	 = _mm_mul_ps(XMM4, pwk2r);
			XMM5	 = _mm_mul_ps(XMM5, pwk2i);
			XMM0	 = _mm_add_ps(XMM0, XMM2);
			XMM4	 = _mm_add_ps(XMM4, XMM5);
			_mm_store_ps(a+j , XMM0);
			_mm_store_ps(a+j2, XMM4);

			XMM0	 = XMM1;
			XMM5	 = XMM3;
			XMM4	 = XMM1;
			XMM5	 = _mm_shuffle_ps(XMM5, XMM5, _MM_SHUFFLE(2,3,0,1));
			XMM4	 = _mm_shuffle_ps(XMM4, XMM4, _MM_SHUFFLE(2,3,0,1));
			XMM2	 = XMM4;
			XMM5	 = _mm_xor_ps(XMM5, PM128(PCS_NRNR));
			XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_NRNR));
			XMM1	 = _mm_add_ps(XMM1, XMM5);
			XMM4	 = _mm_sub_ps(XMM4, XMM3);
			XMM0	 = _mm_sub_ps(XMM0, XMM5);
			XMM2	 = _mm_add_ps(XMM2, XMM3);
			XMM1	 = _mm_mul_ps(XMM1, pwk1r);
			XMM4	 = _mm_mul_ps(XMM4, pwk1i);
			XMM0	 = _mm_mul_ps(XMM0, pwk3r);
			XMM2	 = _mm_mul_ps(XMM2, pwk3i);
			XMM1	 = _mm_add_ps(XMM1, XMM4);
			XMM0	 = _mm_add_ps(XMM0, XMM2);
			_mm_store_ps(a+j1, XMM1);
			_mm_store_ps(a+j3, XMM0);
		}
		wk1r = w[k2 + 2];
		wk1i = w[k2 + 3];
		wk3r = wk1r - 2 * wk2r * wk1i;
		wk3i = 2 * wk2r * wk1r - wk1i;
		pwk1r	 = _mm_set1_ps(wk1r);
		pwk1i	 = _mm_set1_ps(wk1i);
		pwk2r	 = _mm_set1_ps(wk2r);
		pwk2i	 = _mm_set1_ps(wk2i);
		pwk3r	 = _mm_set1_ps(wk3r);
		pwk3i	 = _mm_set1_ps(wk3i);
		pwk1i	 = _mm_xor_ps(pwk1i, PM128(PCS_NRNR));
		pwk2r	 = _mm_xor_ps(pwk2r, PM128(PCS_NRNR));
		pwk3i	 = _mm_xor_ps(pwk3i, PM128(PCS_NRNR));
		for (j = k + m; j < l + (k + m); j += 4) {
			__m128	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5;

			j1 = j	+ l;
			j2 = j1 + l;
			j3 = j2 + l;

			XMM0	 = _mm_load_ps(a+j );
			XMM2	 = _mm_load_ps(a+j2);
			XMM1	 = XMM0;
			XMM3	 = XMM2;
			XMM0	 = _mm_add_ps(XMM0, PM128(a+j1));
			XMM1	 = _mm_sub_ps(XMM1, PM128(a+j1));
			XMM2	 = _mm_add_ps(XMM2, PM128(a+j3));
			XMM3	 = _mm_sub_ps(XMM3, PM128(a+j3));

			XMM4	 = XMM0;
			XMM5	 = XMM0;
			XMM6	 = XMM2;
			XMM5	 = _mm_shuffle_ps(XMM5, XMM5, _MM_SHUFFLE(2,3,0,1));
			XMM6	 = _mm_shuffle_ps(XMM6, XMM6, _MM_SHUFFLE(2,3,0,1));
			XMM4	 = _mm_sub_ps(XMM4, XMM2);
			XMM5	 = _mm_sub_ps(XMM5, XMM6);
			XMM4	 = _mm_mul_ps(XMM4, pwk2i);
			XMM5	 = _mm_mul_ps(XMM5, pwk2r);
			XMM0	 = _mm_add_ps(XMM0, XMM2);
			XMM5	 = _mm_sub_ps(XMM5, XMM4);
			_mm_store_ps(a+j , XMM0);
			_mm_store_ps(a+j2, XMM5);

			XMM0	 = XMM1;
			XMM5	 = XMM3;
			XMM4	 = XMM1;
			XMM5	 = _mm_shuffle_ps(XMM5, XMM5, _MM_SHUFFLE(2,3,0,1));
			XMM4	 = _mm_shuffle_ps(XMM4, XMM4, _MM_SHUFFLE(2,3,0,1));
			XMM2	 = XMM4;
			XMM5	 = _mm_xor_ps(XMM5, PM128(PCS_NRNR));
			XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_NRNR));
			XMM1	 = _mm_add_ps(XMM1, XMM5);
			XMM4	 = _mm_sub_ps(XMM4, XMM3);
			XMM0	 = _mm_sub_ps(XMM0, XMM5);
			XMM2	 = _mm_add_ps(XMM2, XMM3);
			XMM1	 = _mm_mul_ps(XMM1, pwk1r);
			XMM4	 = _mm_mul_ps(XMM4, pwk1i);
			XMM0	 = _mm_mul_ps(XMM0, pwk3r);
			XMM2	 = _mm_mul_ps(XMM2, pwk3i);
			XMM1	 = _mm_add_ps(XMM1, XMM4);
			XMM0	 = _mm_add_ps(XMM0, XMM2);
			_mm_store_ps(a+j1, XMM1);
			_mm_store_ps(a+j3, XMM0);
		}
	}
}


static inline void bitrv2(int n, int * __restrict ip, float * __restrict a)
{
	int j, j1, k, k1, l, m, m2;
	float xr, xi, yr, yi;

	ip[0] = 0;
	l = n;
	m = 1;
	while ((m << 3) < l) {
		l >>= 1;
		for (j = 0; j < m; j++) {
			ip[m + j] = ip[j] + l;
		}
		m <<= 1;
	}
	m2 = 2 * m;
	if ((m << 3) == l) {
		for (k = 0; k < m; k++) {
			for (j = 0; j < k; j++) {
				__m128	X0, Y0, X1, Y1;
				j1 = 2 * j + ip[k];
				k1 = 2 * k + ip[j];
				X0	 = _mm_loadl_pi(X0, (__m64*)(a+j1	  ));
				Y0	 = _mm_loadl_pi(Y0, (__m64*)(a+k1	  ));
				X1	 = _mm_loadl_pi(X1, (__m64*)(a+j1+m2*2));
				Y1	 = _mm_loadl_pi(Y1, (__m64*)(a+k1+m2  ));
				X0	 = _mm_loadh_pi(X0, (__m64*)(a+j1+m2  ));
				Y0	 = _mm_loadh_pi(Y0, (__m64*)(a+k1+m2*2));
				X1	 = _mm_loadh_pi(X1, (__m64*)(a+j1+m2*3));
				Y1	 = _mm_loadh_pi(Y1, (__m64*)(a+k1+m2*3));
				_mm_storel_pi((__m64*)(a+k1		), X0);
				_mm_storel_pi((__m64*)(a+j1		), Y0);
				_mm_storel_pi((__m64*)(a+k1+m2	), X1);
				_mm_storel_pi((__m64*)(a+j1+m2*2), Y1);
				_mm_storeh_pi((__m64*)(a+k1+m2*2), X0);
				_mm_storeh_pi((__m64*)(a+j1+m2	), Y0);
				_mm_storeh_pi((__m64*)(a+k1+m2*3), X1);
				_mm_storeh_pi((__m64*)(a+j1+m2*3), Y1);
			}
			j1 = 2 * k + m2 + ip[k];
			k1 = j1 + m2;
			xr = a[j1];
			xi = a[j1 + 1];
			yr = a[k1];
			yi = a[k1 + 1];
			a[j1] = yr;
			a[j1 + 1] = yi;
			a[k1] = xr;
			a[k1 + 1] = xi;
		}
	} else {
		for (k = 1; k < m; k++) {
			for (j = 0; j < k; j++) {
				__m128	X,	Y;
				j1 = 2 * j + ip[k];
				k1 = 2 * k + ip[j];
				X	 = _mm_loadl_pi(X, (__m64*)(a+j1   ));
				Y	 = _mm_loadl_pi(Y, (__m64*)(a+k1   ));
				X	 = _mm_loadh_pi(X, (__m64*)(a+j1+m2));
				Y	 = _mm_loadh_pi(Y, (__m64*)(a+k1+m2));
				_mm_storel_pi((__m64*)(a+k1	  ), X);
				_mm_storel_pi((__m64*)(a+j1	  ), Y);
				_mm_storeh_pi((__m64*)(a+k1+m2), X);
				_mm_storeh_pi((__m64*)(a+j1+m2), Y);
			}
		}
	}
}

static inline void cftfsub(int n, float * __restrict a, float * __restrict w)
{
	int j, j1, j2, j3, l;

	l = 2;
	if (n > 8) {
		cft1st(n, a, w);
		l = 8;
		while ((l << 2) < n) {
			cftmdl(n, l, a, w);
			l <<= 2;
		}
	}
	if ((l << 2) == n) {
		for (j = 0; j < l; j += 4) {
			__m128	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5;

			j1 = j	+ l;
			j2 = j1 + l;
			j3 = j2 + l;

			XMM0	 = _mm_load_ps(a+j );
			XMM2	 = _mm_load_ps(a+j2);
			XMM1	 = XMM0;
			XMM3	 = XMM2;
			XMM0	 = _mm_add_ps(XMM0, PM128(a+j1));
			XMM1	 = _mm_sub_ps(XMM1, PM128(a+j1));
			XMM2	 = _mm_add_ps(XMM2, PM128(a+j3));
			XMM3	 = _mm_sub_ps(XMM3, PM128(a+j3));
			XMM4	 = XMM0;
			XMM5	 = XMM1;
			XMM3	 = _mm_shuffle_ps(XMM3, XMM3, _MM_SHUFFLE(2,3,0,1));
			XMM0	 = _mm_add_ps(XMM0, XMM2);
			XMM4	 = _mm_sub_ps(XMM4, XMM2);
			XMM3	 = _mm_xor_ps(XMM3, PM128(PCS_NRNR));
			_mm_store_ps(a+j , XMM0);
			_mm_store_ps(a+j2, XMM4);
			XMM1	 = _mm_add_ps(XMM1, XMM3);
			XMM5	 = _mm_sub_ps(XMM5, XMM3);
			_mm_store_ps(a+j1, XMM1);
			_mm_store_ps(a+j3, XMM5);
		}
	} else {
		for (j = 0; j < l; j += 8)
		{
			__m128	XMM0, XMM1, XMM2, XMM3;
			j1 = j + l;

			XMM0	 = _mm_load_ps(a+j	 );
			XMM1	 = _mm_load_ps(a+j+ 4);
			XMM2	 = XMM0;
			XMM3	 = XMM1;
			XMM0	 = _mm_add_ps(XMM0, PM128(a+j1	));
			XMM1	 = _mm_add_ps(XMM1, PM128(a+j1+4));
			XMM2	 = _mm_sub_ps(XMM2, PM128(a+j1	));
			XMM3	 = _mm_sub_ps(XMM3, PM128(a+j1+4));
			_mm_store_ps(a+j   , XMM0);
			_mm_store_ps(a+j +4, XMM1);
			_mm_store_ps(a+j1  , XMM2);
			_mm_store_ps(a+j1+4, XMM3);
		}
	}
}

static inline void rftfsub(int n, float * __restrict a, int nc, float * __restrict c)
{
	int		j, k, kk, ks, m;

	m	 = n >> 1;
	ks	 = 2 * nc / m;
	kk	 = 0;
	j	 = 2;
	{
		float	wkr, wki, xr, xi, yr, yi;
		k	 = n - j;
		kk	+= ks;
		wkr	 = 0.5f - c[nc - kk];
		wki	 = c[kk];
		xr	 = a[j	] - a[k	 ];
		xi	 = a[j+1] + a[k+1];
		yr	 = wkr * xr - wki * xi;
		yi	 = wkr * xi + wki * xr;
		a[j	 ]	-= yr;
		a[j+1]	-= yi;
		a[k	 ]	+= yr;
		a[k+1]	-= yi;
		j	+= 2;
	}
	n	-= 2;
	kk	+= ks;
	for(;j<m;j+=4)
	{
		__m128	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6;
		k	 = n - j;
		XMM0	 = _mm_load_ss(PFV_0P5);
		XMM1	 = _mm_load_ss(c+kk	  );
		XMM6	 = _mm_loadl_pi(XMM6, (__m64*)(a+k+2));
		XMM2	 = XMM0;
		XMM3	 = _mm_load_ss(c+kk+ks);
		XMM4	 = _mm_load_ss(c+nc-kk	 );
		XMM5	 = _mm_load_ss(c+nc-kk-ks);
		XMM6	 = _mm_loadh_pi(XMM6, (__m64*)(a+k	));
		XMM0	 = _mm_sub_ss(XMM0, XMM4);
		XMM2	 = _mm_sub_ss(XMM2, XMM5);
		XMM4	 = XMM6;
		XMM5	 = _mm_load_ps(a+j);
		XMM6	 = _mm_xor_ps(XMM6, PM128(PCS_NRNR));
		XMM0	 = _mm_shuffle_ps(XMM0, XMM2, _MM_SHUFFLE(0,0,0,0));
		XMM1	 = _mm_shuffle_ps(XMM1, XMM3, _MM_SHUFFLE(0,0,0,0));
		XMM6	 = _mm_add_ps(XMM6, XMM5);
		XMM2	 = XMM6;
		XMM2	 = _mm_shuffle_ps(XMM2, XMM2, _MM_SHUFFLE(2,3,0,1));
		XMM6	 = _mm_mul_ps(XMM6, XMM0);
		XMM2	 = _mm_mul_ps(XMM2, XMM1);
		XMM2	 = _mm_xor_ps(XMM2, PM128(PCS_NRNR));
		XMM6	 = _mm_add_ps(XMM6, XMM2);
		XMM0	 = XMM6;
		XMM5	 = _mm_sub_ps(XMM5, XMM6);
		XMM0	 = _mm_xor_ps(XMM0, PM128(PCS_NRNR));
		_mm_store_ps(a+j, XMM5);
		XMM4	 = _mm_sub_ps(XMM4, XMM0);
		_mm_storel_pi((__m64*)(a+k+2), XMM4);
		_mm_storeh_pi((__m64*)(a+k	), XMM4);
		kk	+= ks*2;
	}
}

static inline void cftbsub(int n, float * __restrict a, float * __restrict w)
{
	int j, j1, j2, j3, l;
	float x0r, x0i;

	l = 2;
	if (n > 8) {
		cft1st(n, a, w);
		l = 8;
		while ((l << 2) < n) {
			cftmdl(n, l, a, w);
			l <<= 2;
		}
	}

	if ((l << 2) == n) {
		for (j = 0; j < l; j += 2) {
			j1 = j	+ l;
			j2 = j1 + l;
			j3 = j2 + l;

			__m128 j2j0, j3j1, x1x0, x3x2;

			j2j0 = _mm_loadl_pi(j2j0, (__m64*)(a+j	));
			j2j0 = _mm_loadh_pi(j2j0, (__m64*)(a+j2 ));
			j3j1 = _mm_loadl_pi(j3j1, (__m64*)(a+j1 ));
			j3j1 = _mm_loadh_pi(j3j1, (__m64*)(a+j3 ));

			x1x0 = _mm_add_ps(
					_mm_xor_ps(_mm_shuffle_ps(j2j0, j2j0, _MM_SHUFFLE(1, 0, 1, 0)), PM128(PCS_RNRN)),
					_mm_xor_ps(_mm_shuffle_ps(j3j1, j3j1, _MM_SHUFFLE(1, 0, 1, 0)), PM128(PCS_NRRN))
					);

			x3x2 = _mm_add_ps(
							   _mm_shuffle_ps(j2j0, j2j0, _MM_SHUFFLE(3, 2, 3, 2)),
					_mm_xor_ps(_mm_shuffle_ps(j3j1, j3j1, _MM_SHUFFLE(3, 2, 3, 2)), PM128(PCS_RRNN))
					);

			__m128 t, m;
			__m128 x3r_x3i_x2i_x2r =
				_mm_shuffle_ps(x3x2, x3x2, _MM_SHUFFLE(2,3,1,0));


			m = _mm_xor_ps(x3r_x3i_x2i_x2r, PM128(PCS_NNNR));
			t = _mm_sub_ps(x1x0, m);
			_mm_storel_pi((__m64*)(a + j ), t);
			_mm_storeh_pi((__m64*)(a + j1), t);

			m = _mm_xor_ps(x3r_x3i_x2i_x2r, PM128(PCS_NNNR));
			t = _mm_add_ps(x1x0, m);
			_mm_storel_pi((__m64*)(a + j2), t);
			_mm_storeh_pi((__m64*)(a + j3), t);
		}
	} else {
		for (j = 0; j < l; j += 2) {
			j1 = j + l;

			x0r =  a[j	   ] - (+ a[j1	  ]);
			x0i = -a[j	+ 1] - (- a[j1 + 1]);
			a[j		] =	 a[j	 ] + a[j1	 ];
			a[j	 + 1] = -a[j  + 1] - a[j1 + 1];
			a[j1	] = x0r;
			a[j1 + 1] = x0i;
		}
	}
}

static void rftbsub(int n, float * __restrict a, int nc, float * __restrict c)
{
	int j, k, kk, ks, m;
	float wkr, wki, xr, xi, yr, yi;

	a[1] = -a[1];
	m = n >> 1;
	ks = 2 * nc / m;
	kk = 0;
	for (j = 2; j < m; j += 2) {
		k = n - j;
		kk += ks;
		wkr = 0.5f - c[nc - kk];
		wki = c[kk];

		xr = a[j	] - a[k	   ];
		xi = a[j + 1] + a[k + 1];

		yr = wkr * xr + wki * xi;
		yi = wkr * xi - wki * xr;

		a[j	   ]  = -yr + a[j	 ];
		a[j + 1]  =	 yi - a[j + 1];
		a[k	   ]  =	 yr + a[k	 ];
		a[k + 1]  =	 yi - a[k + 1];

	}
	a[m + 1] = -a[m + 1];
}


#include <math.h>

static void makewt(int nw, int * __restrict ip, float * __restrict w)
{
	int j, nwh;
	float delta, x, y;

	ip[0] = nw;
	ip[1] = 1;
	if (nw > 2) {
		nwh = nw >> 1;
		delta = (float)(atan(1.0) / nwh);
		w[0] = 1;
		w[1] = 0;
		w[nwh] = cos(delta * nwh);
		w[nwh + 1] = w[nwh];
		if (nwh > 2) {
			for (j = 2; j < nwh; j += 2) {
				x = cos(delta * j);
				y = sin(delta * j);
				w[j] = x;
				w[j + 1] = y;
				w[nw - j] = y;
				w[nw - j + 1] = x;
			}
			bitrv2(nw, ip + 2, w);
		}
	}
}


static void makect(int nc, int *ip, float *c)
{
	int j, nch;
	float delta;

	ip[1] = nc;
	if (nc > 1) {
		nch = nc >> 1;
		delta = (float)(atan(1.0) / nch);
		c[0] = (float)(cos(delta * nch));
		c[nch] = 0.5f * c[0];
		for (j = 1; j < nch; j++) {
			c[j] = (float)(0.5 * cos(delta * j));
			c[nc - j] = (float)(0.5 * sin(delta * j));
		}
	}
}

void rdft_neon(int n, int isgn, float * __restrict a, int * __restrict ip, float * __restrict w)
{
	int nw, nc;
	float xi;

	nw = ip[0];
	if (n > (nw << 2)) {
		nw = n >> 2;
		makewt(nw, ip, w);
	}
	nc = ip[1];
	if (n > (nc << 2)) {
		nc = n >> 2;
		makect(nc, ip, w + nw);
	}
	if (isgn >= 0) {
		if (n > 4) {
			bitrv2(n, ip + 2, a);
			cftfsub(n, a, w);
			rftfsub(n, a, nc, w + nw);
		} else if (n == 4) {
			cftfsub(n, a, w);
		}
		xi = a[0] - a[1];
		a[0] += a[1];
		a[1] = xi;
	} else {
		a[1] = 0.5f * (a[0] - a[1]);
		a[0] -= a[1];
		if (n > 4) {
			rftbsub(n, a, nc, w + nw);
			bitrv2(n, ip + 2, a);
			cftbsub(n, a, w);
		} else if (n == 4) {
			cftfsub(n, a, w);
		}
	}
}


//---------------------------------------------------------------------------
#endif // TVP_SOUND_HAS_ARM_SIMD
