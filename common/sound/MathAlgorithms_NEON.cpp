//---------------------------------------------------------------------------
/*
	Risa [りさ]      alias 吉里吉里3 [kirikiri-3]
	 stands for "Risa Is a Stagecraft Architecture"
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
//! @file
//! @brief 数学関数群 (ARM NEON 版)
//---------------------------------------------------------------------------

#include "tjsCommHead.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include "MathAlgorithms.h"

#if defined(TVP_SOUND_HAS_ARM_SIMD)
#include <arm_neon.h>

//---------------------------------------------------------------------------
/**
 * 窓関数を適用しながらのインターリーブ解除 (NEON 版)
 *
 * C 参照実装 (MathAlgorithms.cpp::DeinterleaveApplyingWindow) との byte-exact
 * 一致を保つため、fused-multiply-add (vfmaq_f32) は使わず、vmulq_f32 +
 * vaddq_f32 (または単一 vmulq) の 2 段演算として実装する。
 *
 * mono  : dest[0][n]     = src[n] * win[n]
 * stereo: dest[0..1][n]  = src[n*2..n*2+1] * win[n]
 * その他: スカラー C 版にフォールバック
 */
void DeinterleaveApplyingWindow_neon(float * __restrict dest[], const float * __restrict src,
					float * __restrict win, int numch, size_t destofs, size_t len)
{
	size_t n;
	switch (numch)
	{
	case 1: // mono
		{
			float * dest0 = dest[0] + destofs;
			n = 0;
			// 4 sample/iter
			for (; n + 4 <= len; n += 4)
			{
				float32x4_t s = vld1q_f32(src + n);
				float32x4_t w = vld1q_f32(win + n);
				vst1q_f32(dest0 + n, vmulq_f32(s, w));
			}
			// tail
			for (; n < len; n++)
			{
				dest0[n] = src[n] * win[n];
			}
		}
		break;

	case 2: // stereo
		{
			float * dest0 = dest[0] + destofs;
			float * dest1 = dest[1] + destofs;
			n = 0;
			// 4 frame/iter = 8 interleaved samples。vld2q_f32 が LR をネイティブに分解
			for (; n + 4 <= len; n += 4)
			{
				float32x4x2_t s = vld2q_f32(src + n * 2);
				float32x4_t   w = vld1q_f32(win + n);
				vst1q_f32(dest0 + n, vmulq_f32(s.val[0], w));
				vst1q_f32(dest1 + n, vmulq_f32(s.val[1], w));
			}
			// tail
			for (; n < len; n++)
			{
				dest0[n] = src[n * 2 + 0] * win[n];
				dest1[n] = src[n * 2 + 1] * win[n];
			}
		}
		break;

	default:
		// generic: スカラー C 版に委譲
		DeinterleaveApplyingWindow(dest, src, win, numch, destofs, len);
		break;
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * 窓関数を適用しながらのインターリーブ+オーバーラッピング (NEON 版)
 *
 * accumulator 系: dest[n] += src[ch][n] * win[n]。AArch64 の vfmaq_f32 は
 * fused-multiply-add なので丸めが 1 段になり、スカラー C 版 (mul + add =
 * 2 段丸め) と byte-exact には一致しない。byte-exact 維持のため vmulq_f32 +
 * vaddq_f32 の 2 段演算として実装する。
 */
void InterleaveOverlappingWindow_neon(float * __restrict dest,
	const float * __restrict const * __restrict src,
	float * __restrict win, int numch, size_t srcofs, size_t len)
{
	size_t n;
	switch (numch)
	{
	case 1: // mono
		{
			const float * src0 = src[0] + srcofs;
			n = 0;
			for (; n + 4 <= len; n += 4)
			{
				float32x4_t d = vld1q_f32(dest + n);
				float32x4_t s = vld1q_f32(src0 + n);
				float32x4_t w = vld1q_f32(win + n);
				float32x4_t p = vmulq_f32(s, w);
				vst1q_f32(dest + n, vaddq_f32(d, p));
			}
			for (; n < len; n++)
			{
				dest[n] += src0[n] * win[n];
			}
		}
		break;

	case 2: // stereo
		{
			const float * src0 = src[0] + srcofs;
			const float * src1 = src[1] + srcofs;
			n = 0;
			for (; n + 4 <= len; n += 4)
			{
				// dest は interleaved なので vld2q + vst2q で LR を分離
				float32x4x2_t d  = vld2q_f32(dest + n * 2);
				float32x4_t   s0 = vld1q_f32(src0 + n);
				float32x4_t   s1 = vld1q_f32(src1 + n);
				float32x4_t   w  = vld1q_f32(win + n);
				d.val[0] = vaddq_f32(d.val[0], vmulq_f32(s0, w));
				d.val[1] = vaddq_f32(d.val[1], vmulq_f32(s1, w));
				vst2q_f32(dest + n * 2, d);
			}
			for (; n < len; n++)
			{
				dest[n * 2 + 0] += src0[n] * win[n];
				dest[n * 2 + 1] += src1[n] * win[n];
			}
		}
		break;

	default:
		// generic: スカラー C 版に委譲
		InterleaveOverlappingWindow(dest, src, win, numch, srcofs, len);
		break;
	}
}
//---------------------------------------------------------------------------

#endif // TVP_SOUND_HAS_ARM_SIMD
