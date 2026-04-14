/*
	Portable CPU feature detection entry point.

	`TVPInitCPUFeatures()` is the single public entry that any platform-side
	startup code (Win32 SysInitImpl, Generic/SDL3 SysInitImpl, harness main)
	should call exactly once before invoking the SIMD init dispatchers
	(`TVPGL_SSE2_Init()`, `TVPGL_NEON_Init()`, etc.).

	The function populates the global `TVPCPUType` mask using:
	  - x86 (any OS): cpuid via `TVPCheckCPU()` (common/visual/IA32/detect_cpu.cpp),
	    then copies `TVPCPUFeatures` into `TVPCPUType`. AVX/AVX2 OS-support
	    validation is platform-portable (MSVC SEH `__xgetbv` on Windows,
	    `__builtin_cpu_supports` on GCC/Clang).
	  - ARM64 (any OS): NEON/ASIMD are mandatory baseline, set unconditionally.
	  - ARM32 Linux/Android: probe via `getauxval(AT_HWCAP) & HWCAP_NEON`.
	  - ARM32 other: assume NEON (Cortex-A series since 2009 has it ubiquitously).
	  - Other archs: leave `TVPCPUType` at 0 (= pure C reference fallback).

	Win32 specifically still has its own `TVPDetectCPU()` (per-CPU thread
	walker that reports each core's features and dumps log) which can run
	*after* `TVPInitCPUFeatures()` for additional logging / validation.
*/

#ifndef __TVP_CPU_DETECT_H__
#define __TVP_CPU_DETECT_H__

#include "tjsTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

extern tjs_uint32 TVPCPUType;

/*
 * Populate TVPCPUType with the running CPU's feature set.
 * Idempotent: calling multiple times is safe (re-detects).
 */
void TVPInitCPUFeatures();

#ifdef __cplusplus
}

/*
 * Apply command-line overrides (-cpusse2=no, -cpuavx2=force, etc.) to
 * TVPCPUType.  Must be called after TVPInitCPUFeatures() and after the
 * command-line parser is ready (TVPGetCommandLine must work).
 *
 * This was originally Win32-only (DetectCPU.cpp) but is now portable so
 * that the Generic/SDL3 build can also override CPU features.
 */
void TVPApplyCPUFeatureOverrides();
#endif

#endif /* __TVP_CPU_DETECT_H__ */
