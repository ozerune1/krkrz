/******************************************************************************/
/**
 * CPU情報を取得する
 * ----------------------------------------------------------------------------
 * 	Copyright (C) T.Imoto <http://www.kaede-software.com>
 * ----------------------------------------------------------------------------
 * @author		T.Imoto
 * @date		2015/12/22
 * @note
 *****************************************************************************/


#include "tjsCommHead.h"
#include "tvpgl_ia32_intf.h"

/* このファイルは x86 / x86_64 専用 (cpuid を使う)。
   sources.cmake では KRKRZ_TARGET_X86 のときだけ KRKRZ_SRC に追加されるが、
   うっかり ARM ビルドに混入しても安全に no-op になるよう内部で gating する。 */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)

#ifdef _MSC_VER
#include <windows.h>
#include <intrin.h>
extern "C" unsigned __int64 __xgetbv(int);
static bool __os_has_avx_support() {
	// Check if the OS will save the YMM registers
	__try {
		unsigned long long xcrFeatureMask = __xgetbv(_XCR_XFEATURE_ENABLED_MASK);
		return (xcrFeatureMask & 6) == 6;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}
#else
#include <cpuid.h>
#include <x86intrin.h>
static inline void krkrz_cpuid(int CPUInfo[4],int InfoType) {
  __cpuid(InfoType, CPUInfo[0], CPUInfo[1], CPUInfo[2], CPUInfo[3]);
}
static inline void krkrz_cpuidex(int CPUInfo[4],int InfoType,int ECXValue) {
  __cpuid_count(InfoType, ECXValue, CPUInfo[0], CPUInfo[1], CPUInfo[2], CPUInfo[3]);
}
#undef __cpuid
#define __cpuid(info, op)          krkrz_cpuid((info), (op))
#define __cpuidex(info, op, ecxv)  krkrz_cpuidex((info), (op), (ecxv))
/* AVX/AVX2 の OS-support 検査は GCC/Clang の組み込みで OS の YMM 保存ビットを
   含めて判定してくれる (内部で xgetbv を呼ぶ)。__builtin_cpu_init() を 1 度
   叩いてから __builtin_cpu_supports("avx") / "avx2" を使う。 */
static bool __os_has_avx_support() {
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx") != 0;
}
static bool __os_has_avx2_support() {
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx2") != 0;
}
#endif

extern "C" {
tjs_uint32 TVPCPUFeatures;
tjs_uint32 TVPCPUID1_EAX;
tjs_uint32 TVPCPUID1_EBX;
tjs_nchar TVPCPUVendor[16];
tjs_nchar TVPCPUName[52];
tjs_uint32 TVPCPUPhysicalCore;
extern tjs_uint32 TVPCPUType;
}

static void GetCpuid( int op, int& eax, int& ebx, int& ecx, int& edx) {
	int info[4] = {0,0,0,0};
	__cpuid( info, op );
	eax = info[0];
	ebx = info[1];
	ecx = info[2];
	edx = info[3];
}
static void GetCpuidEx( int op, int& eax, int& ebx, int& ecx, int& edx, int ecxv ) {
	int info[4] = {0,0,0,0};
	__cpuidex( info, op, ecxv );
	eax = info[0];
	ebx = info[1];
	ecx = info[2];
	edx = info[3];
}

static int GetCpuVendor( int& max )
{
	int ret = TVP_CPU_IS_UNKNOWN;
	int eax, ebx, ecx, edx;
	GetCpuid( 0, eax, ebx, ecx, edx );
	max = eax;

#define MAKE_VENDOR_CODE( a, b, c, d )	( ((d) << 24) | ((c) << 16) | ((b) << 8) | (a) )
	// GenuineIntel
	static const int Intel_EBX = MAKE_VENDOR_CODE( 'G', 'e', 'n', 'u' );
	static const int Intel_EDX = MAKE_VENDOR_CODE( 'i', 'n', 'e', 'I' );
	static const int Intel_ECX = MAKE_VENDOR_CODE( 'n', 't', 'e', 'l' );

	// AuthenticAMD
	static const int AMD_EBX = MAKE_VENDOR_CODE( 'A', 'u', 't', 'h' );
	static const int AMD_EDX = MAKE_VENDOR_CODE( 'e', 'n', 't', 'i' );
	static const int AMD_ECX = MAKE_VENDOR_CODE( 'c', 'A', 'M', 'D' );

	// GenuineTMx86
	static const int Transmeta_EBX = MAKE_VENDOR_CODE( 'G', 'e', 'n', 'u' );
	static const int Transmeta_EDX = MAKE_VENDOR_CODE( 'i', 'n', 'e', 'T' );
	static const int Transmeta_ECX = MAKE_VENDOR_CODE( 'M', 'x', '8', '6' );

	// CyrixInstead
	static const int Cyrix_EBX = MAKE_VENDOR_CODE( 'C', 'y', 'r', 'i' );
	static const int Cyrix_EDX = MAKE_VENDOR_CODE( 'x', 'I', 'n', 's' );
	static const int Cyrix_ECX = MAKE_VENDOR_CODE( 't', 'e', 'a', 'd' );

	// CentaurHauls
	static const int IDT_EBX = MAKE_VENDOR_CODE( 'C', 'e', 'n', 't' );
	static const int IDT_EDX = MAKE_VENDOR_CODE( 'a', 'u', 'r', 'H' );
	static const int IDT_ECX = MAKE_VENDOR_CODE( 'a', 'u', 'l', 's' );

	// RiseRiseRise
	static const int Rise_EBX = MAKE_VENDOR_CODE( 'R', 'i', 's', 'e' );
	static const int Rise_EDX = MAKE_VENDOR_CODE( 'R', 'i', 's', 'e' );
	static const int Rise_ECX = MAKE_VENDOR_CODE( 'R', 'i', 's', 'e' );

	// NexGenDriven
	static const int NexGen_EBX = MAKE_VENDOR_CODE( 'N', 'e', 'x', 'G' );
	static const int NexGen_EDX = MAKE_VENDOR_CODE( 'e', 'n', 'D', 'r' );
	static const int NexGen_ECX = MAKE_VENDOR_CODE( 'i', 'v', 'e', 'n' );

	// UMC UMC UMC
	static const int UMC_EBX = MAKE_VENDOR_CODE( 'U', 'M', 'C', ' ' );
	static const int UMC_EDX = MAKE_VENDOR_CODE( 'U', 'M', 'C', ' ' );
	static const int UMC_ECX = MAKE_VENDOR_CODE( 'U', 'M', 'C', ' ' );

	// Geode By NSC
	static const int NSC_EBX = MAKE_VENDOR_CODE( 'G', 'e', 'o', 'd' );
	static const int NSC_EDX = MAKE_VENDOR_CODE( 'e', ' ', 'B', 'y' );
	static const int NSC_ECX = MAKE_VENDOR_CODE( 'N', 'S', 'C', ' ' );

	// Compaq FX32!
	static const int Compaq_EBX = MAKE_VENDOR_CODE( 'C', 'o', 'm', 'p' );
	static const int Compaq_EDX = MAKE_VENDOR_CODE( 'a', 'q', ' ', 'F' );
	static const int Compaq_ECX = MAKE_VENDOR_CODE( 'X', '3', '2', '!' );
#undef MAKE_VENDOR_CODE

	if( ebx == Intel_EBX && edx == Intel_EDX && ecx == Intel_ECX ) {
		ret = TVP_CPU_IS_INTEL;
	} else if( ebx == AMD_EBX && edx == AMD_EDX && ecx == AMD_ECX ) {
		ret = TVP_CPU_IS_AMD;
	} else if( ebx == Transmeta_EBX && edx == Transmeta_EDX && ecx == Transmeta_ECX ) {
		ret = TVP_CPU_IS_TRANSMETA;
	} else if( ebx == Cyrix_EBX && edx == Cyrix_EDX && ecx == Cyrix_ECX ) {
		ret = TVP_CPU_IS_CYRIX;
	} else if( ebx == IDT_EBX && edx == IDT_EDX && ecx == IDT_ECX ) {
		ret = TVP_CPU_IS_IDT;
	} else if( ebx == Rise_EBX && edx == Rise_EDX && ecx == Rise_ECX ) {
		ret = TVP_CPU_IS_RISE;
	} else if( ebx == NexGen_EBX && edx == NexGen_EDX && ecx == NexGen_ECX ) {
		ret = TVP_CPU_IS_NEXGEN;
	} else if( ebx == UMC_EBX && edx == UMC_EDX && ecx == UMC_ECX ) {
		ret = TVP_CPU_IS_UMC;
	} else if( ebx == NSC_EBX && edx == NSC_EDX && ecx == NSC_ECX ) {
		ret = TVP_CPU_IS_NSC;
	} else if( ebx == Compaq_EBX && edx == Compaq_EDX && ecx == Compaq_ECX ) {
		ret = TVP_CPU_IS_COMPAQ;
	}
	int* vendor = (int*)TVPCPUVendor;
	vendor[0] = ebx;
	vendor[1] = edx;
	vendor[2] = ecx;
	return ret;
}
//---------------------------------------------------------------------------
static void GetCPUName() {
	int eax, ebx, ecx, edx;
	GetCpuid( 0x80000000, eax, ebx, ecx, edx );
	if( eax >= 0x80000004 ) {
		int* name = (int*)TVPCPUName;
		GetCpuid( 0x80000002, eax, ebx, ecx, edx );
		name[0] = eax;
		name[1] = ebx;
		name[2] = ecx;
		name[3] = edx;
		GetCpuid( 0x80000003, eax, ebx, ecx, edx );
		name[4] = eax;
		name[5] = ebx;
		name[6] = ecx;
		name[7] = edx;
		GetCpuid( 0x80000004, eax, ebx, ecx, edx );
		name[8] = eax;
		name[9] = ebx;
		name[10] = ecx;
		name[11] = edx;
	}
}
//---------------------------------------------------------------------------
// TVPCheckCPU
//---------------------------------------------------------------------------
tjs_uint32 TVPCheckCPU()
{
	TVPCPUFeatures = TVPCPUID1_EAX = TVPCPUID1_EBX = 0;
	TVPCPUPhysicalCore = 1;
	memset( TVPCPUVendor, 0, sizeof(TVPCPUVendor) );
	memset( TVPCPUName, 0, sizeof(TVPCPUName) );

	int maxCpuId = 0;
	int vendor = GetCpuVendor( maxCpuId );

	unsigned long flags = 0;

	int eax, ebx, ecx, edx;
	GetCpuid( 0x80000000, eax, ebx, ecx, edx );
	int maxCpuIdEx = eax;

	int featureEx = 0, featureExEcx = 0;
	if( maxCpuIdEx > 0x80000000 ) {
		GetCpuid( 0x80000001, eax, ebx, ecx, edx );
		featureExEcx = ecx;
		featureEx = edx;

		if( featureEx & 1 ) flags |= TVP_CPU_HAS_FPU;
		if( featureEx & (1<<22) ) flags |= TVP_CPU_HAS_EMMX;
		if( featureEx & (1<<27) ) flags |= TVP_CPU_HAS_TSCP;
		if( featureEx & (1<<31) ) flags |= TVP_CPU_HAS_3DN;
		if( featureEx & (1<<30) ) flags |= TVP_CPU_HAS_E3DN;
		if( featureExEcx & (1<<6) ) flags |= TVP_CPU_HAS_SSE4a;
	}

	GetCpuid(1,eax,ebx,ecx,edx);
	TVPCPUID1_EAX = eax;
	TVPCPUID1_EBX = ebx;
	int featureEcx = ecx;
	int feature = edx;

	if( feature & 1 ) flags |= TVP_CPU_HAS_FPU;
	if( feature & (1<<23) ) flags |= TVP_CPU_HAS_MMX;
	if( feature & (1<<15) ) flags |= TVP_CPU_HAS_CMOV;
	if( feature & (1<<25) ) {
		if( vendor == TVP_CPU_IS_INTEL ) flags |= TVP_CPU_HAS_EMMX|TVP_CPU_HAS_SSE;
		else  flags |= TVP_CPU_HAS_SSE;
	}
	if( feature & (1<<26) ) flags |= TVP_CPU_HAS_SSE2;
	if( featureEcx & 1 ) flags |= TVP_CPU_HAS_SSE3;
	if( featureEcx & (1<<9) ) flags |= TVP_CPU_HAS_SSSE3;
	if( featureEcx & (1<<12) ) flags |= TVP_CPU_HAS_FMA3;
	if( featureEcx & (1<<19) ) flags |= TVP_CPU_HAS_SSE41;
	if( featureEcx & (1<<20) ) flags |= TVP_CPU_HAS_SSE42;
	if( featureEcx & (1<<25) ) flags |= TVP_CPU_HAS_AES;
	if( featureEcx & (1<<28) ) flags |= TVP_CPU_HAS_AVX;
	if( featureEcx & (1<<30) ) flags |= TVP_CPU_HAS_RDRAND;
	if( feature & (1<<4) ) flags |= TVP_CPU_HAS_TSC;

	GetCpuidEx( 7, eax, ebx, ecx, edx, 0 );
	int featureEbx = ebx;
	if( featureEbx & (1<<5) ) flags |= TVP_CPU_HAS_AVX2;
	if( featureEbx & (1<<18) ) flags |= TVP_CPU_HAS_RDSEED;

	if( vendor == TVP_CPU_IS_INTEL && maxCpuId >= 0x00000004 ) {
		GetCpuid( 0x00000004, eax, ebx, ecx, edx );
		TVPCPUPhysicalCore = ((eax >> 26) & 0x3F) + 1;
	}

	if( vendor == TVP_CPU_IS_AMD && maxCpuIdEx >= 0x80000008 ) {
		GetCpuid( 0x80000008, eax, ebx, ecx, edx );
		TVPCPUPhysicalCore = (ecx & 0xFF) + 1;
	}
	GetCPUName();

	// OS Check
#ifdef _MSC_VER
	if( flags & (TVP_CPU_HAS_AVX|TVP_CPU_HAS_AVX2) ) {
		// YMMレジスタ(AVX)はWindowsなら7 SP1以降
		if( !__os_has_avx_support() ) {
			flags &= ~(TVP_CPU_HAS_AVX|TVP_CPU_HAS_AVX2);
		}
	}
#else
	/* GCC/Clang: __builtin_cpu_supports は OS の YMM 保存サポートも見て判定する。
	   AVX フラグが立っているのに OS が未対応なら落とす。AVX2 も同様。 */
	if( (flags & TVP_CPU_HAS_AVX) && !__os_has_avx_support() ) {
		flags &= ~(TVP_CPU_HAS_AVX|TVP_CPU_HAS_AVX2);
	}
	if( (flags & TVP_CPU_HAS_AVX2) && !__os_has_avx2_support() ) {
		flags &= ~TVP_CPU_HAS_AVX2;
	}
#endif

	TVPCPUFeatures = flags | vendor;
	return flags;
}


tjs_uint64 TVPGetTSC() {
	if( TVPCPUType & TVP_CPU_HAS_TSC ) {
		return __rdtsc();
	}
	return 0;
}

tjs_uint64 TVPGetTSCP( tjs_uint32 *aux ) {
	if( TVPCPUType & TVP_CPU_HAS_TSCP ) {
		return __rdtscp(aux);
	}
	return 0;
}

#endif /* x86 / x86_64 */
