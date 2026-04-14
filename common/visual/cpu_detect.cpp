/*
	Portable CPU feature detection implementation. See cpu_detect.h for the
	contract and rationale.

	This file is always built (added to KRKRZ_SRC unconditionally) and uses
	`#if` arch detection internally so it compiles cleanly on every target
	without needing CMake-side gating.
*/

#include "tjsCommHead.h"
#include "tvpgl.h"
#include "cpu_detect.h"
#include "tvpgl_ia32_intf.h" /* for TVP_CPU_HAS_* flag definitions */
#ifndef KRKRZ_STANDALONE_TEST
#include "SysInitIntf.h"     /* TVPGetCommandLine */
#include "DebugIntf.h"       /* TVPAddImportantLog */
#endif

#if defined(__linux__) || defined(__ANDROID__)
#  if defined(__arm__) || defined(__aarch64__)
#    include <sys/auxv.h>
#    include <asm/hwcap.h>
#  endif
#endif

extern "C" {
	/* x86 detection lives in common/visual/IA32/detect_cpu.cpp which is
	   compiled only on KRKRZ_TARGET_X86. We forward-declare its symbols
	   here and call them only inside the x86 #if branch below. */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
	extern tjs_uint32 TVPCheckCPU();
	extern tjs_uint32 TVPCPUFeatures;
#endif
}

void TVPInitCPUFeatures()
{
	TVPCPUType = 0;

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
	/* x86 / x86_64: cpuid をかけて vendor + features を取得する。
	   common/visual/IA32/detect_cpu.cpp に portable cpuid (MSVC は __cpuid、
	   GCC/Clang は asm) があるのでそれをそのまま使う。AVX/AVX2 の OS-support
	   検査もそちら側で対応している。 */
	TVPCheckCPU();
	TVPCPUType = TVPCPUFeatures;

#elif defined(__aarch64__) || defined(_M_ARM64)
	/* ARM64 (AArch64): NEON / ASIMD は ARMv8 の baseline で必須なので無条件に
	   立てる。Linux/macOS/Windows ARM64 すべて同様。 */
	TVPCPUType = TVP_CPU_HAS_ARM_NEON | TVP_CPU_HAS_ARM64_ASIMD;

#elif defined(__arm__) || defined(_M_ARM)
	/* ARMv7 (32bit): NEON は optional だが Cortex-A 系では 2009 以降ほぼ
	   必須。Linux/Android では HWCAP を引いて精密に判定、それ以外の OS では
	   NEON 必須として扱う。 */
#  if (defined(__linux__) || defined(__ANDROID__)) && defined(AT_HWCAP)
	{
		unsigned long hwcap = getauxval(AT_HWCAP);
#    ifdef HWCAP_NEON
		if (hwcap & HWCAP_NEON) {
			TVPCPUType |= TVP_CPU_HAS_ARM_NEON;
		}
#    else
		/* HWCAP_NEON が定義されていない (古い kernel header) → fallback */
		TVPCPUType |= TVP_CPU_HAS_ARM_NEON;
#    endif
	}
#  else
	TVPCPUType |= TVP_CPU_HAS_ARM_NEON;
#  endif

#else
	/* 未対応アーキテクチャ: TVPCPUType = 0 のまま (= C リファレンス fallback) */

#endif
}

// ---------------------------------------------------------------------------
// TVPApplyCPUFeatureOverrides
//
// TJS2 ランタイム (TVPGetCommandLine, tTJSVariant 等) に依存するため、
// スタンドアロンテスト (KRKRZ_STANDALONE_TEST) ではコンパイルしない。
// ---------------------------------------------------------------------------
#ifndef KRKRZ_STANDALONE_TEST

static void TVPDisableCPU(tjs_uint32 featurebit, const tjs_char *name)
{
	tTJSVariant val;
	ttstr str;
	if(TVPGetCommandLine(name, &val))
	{
		str = val;
		if(str == TJS_W("no"))
			TVPCPUType &=~ featurebit;
		else if(str == TJS_W("force"))
			TVPCPUType |= featurebit;
	}
}
//---------------------------------------------------------------------------
void TVPApplyCPUFeatureOverrides()
{
	tjs_uint32 before = TVPCPUType;

	// -cpusimd=no で SIMD 処理全般を無効化 (C リファレンス fallback)
	{
		tTJSVariant val;
		if(TVPGetCommandLine(TJS_W("-cpusimd"), &val))
		{
			ttstr str(val);
			if(str == TJS_W("no"))
			{
				// vendor/family ビットだけ残して feature ビットを全クリア
				TVPCPUType &= TVP_CPU_VENDOR_MASK;
				TVPAddImportantLog(TJS_W("(info) All SIMD features disabled by -cpusimd=no"));
			}
		}
	}

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
	// x86 feature overrides
	TVPDisableCPU(TVP_CPU_HAS_MMX,   TJS_W("-cpummx"));
	TVPDisableCPU(TVP_CPU_HAS_3DN,   TJS_W("-cpu3dn"));
	TVPDisableCPU(TVP_CPU_HAS_SSE,   TJS_W("-cpusse"));
	TVPDisableCPU(TVP_CPU_HAS_CMOV,  TJS_W("-cpucmov"));
	TVPDisableCPU(TVP_CPU_HAS_E3DN,  TJS_W("-cpue3dn"));
	TVPDisableCPU(TVP_CPU_HAS_EMMX,  TJS_W("-cpuemmx"));
	TVPDisableCPU(TVP_CPU_HAS_SSE2,  TJS_W("-cpusse2"));
	TVPDisableCPU(TVP_CPU_HAS_SSE3,  TJS_W("-cpusse3"));
	TVPDisableCPU(TVP_CPU_HAS_SSSE3, TJS_W("-cpussse3"));
	TVPDisableCPU(TVP_CPU_HAS_SSE41, TJS_W("-cpusse41"));
	TVPDisableCPU(TVP_CPU_HAS_SSE42, TJS_W("-cpusse42"));
	TVPDisableCPU(TVP_CPU_HAS_SSE4a, TJS_W("-cpusse4a"));
	TVPDisableCPU(TVP_CPU_HAS_AVX,   TJS_W("-cpuavx"));
	TVPDisableCPU(TVP_CPU_HAS_AVX2,  TJS_W("-cpuavx2"));
	TVPDisableCPU(TVP_CPU_HAS_FMA3,  TJS_W("-cpufma3"));
	TVPDisableCPU(TVP_CPU_HAS_AES,   TJS_W("-cpuaes"));
#endif

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
	// ARM feature overrides
	TVPDisableCPU(TVP_CPU_HAS_ARM_NEON,     TJS_W("-cpuneon"));
#if defined(__aarch64__) || defined(_M_ARM64)
	TVPDisableCPU(TVP_CPU_HAS_ARM64_ASIMD,  TJS_W("-cpuasimd"));
#endif
#endif

	if(before != TVPCPUType)
	{
		TVPAddImportantLog(TJS_W("(info) CPU feature flags overridden by command-line option"));
	}
}

#endif /* !KRKRZ_STANDALONE_TEST */
