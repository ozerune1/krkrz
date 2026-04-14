# ----------------------------------------------------------------------------
# Phase 3: SIMD パリティテスト
#
# 画像処理 SIMD (SSE2/AVX2/NEON) の出力が C リファレンス実装とバイト一致するか
# を検証するスタンドアロンテスト。本体 krkrz とは独立にビルドされ、必要な
# SIMD ソースだけを直接コンパイルする。これにより SDL3 / vcpkg / 描画 / 音響
# などの重たい依存を持ち込まずに「SIMD コアだけ」を検証できる。
#
# 対応プラットフォーム:
#   - x86 / x86_64 (SSE2 + AVX2)  ← 現状の主対象
#   - ARM / ARM64                 ← Phase 1 で NEON が増えたら有効化する
# ----------------------------------------------------------------------------

if (NOT (KRKRZ_TARGET_X86 OR KRKRZ_TARGET_ARM))
    message(STATUS "krkrz_simd_parity_test: skip (architecture not supported)")
    return()
endif()

set(SIMD_TEST_TARGET krkrz_simd_parity_test)

set(SIMD_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/simd_parity_test.cpp
    ${CMAKE_SOURCE_DIR}/common/visual/tvpgl.c
    ${CMAKE_SOURCE_DIR}/common/visual/gl/blend_function.cpp
    ${CMAKE_SOURCE_DIR}/common/visual/cpu_detect.cpp
)

if (KRKRZ_TARGET_X86)
    list(APPEND SIMD_TEST_SRC
        ${CMAKE_SOURCE_DIR}/common/visual/gl/blend_function_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/blend_function_avx2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/adjust_color_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/boxblur_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/colorfill_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/colorfill_avx2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/colormap_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/colormap_avx2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/pixelformat_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/tlg_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/univtrans_sse2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/x86simdutil.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/x86simdutilAVX2.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/IA32/detect_cpu.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/IA32/tvpgl_ia32_intf.c
    )
endif()

if (KRKRZ_TARGET_ARM)
    list(APPEND SIMD_TEST_SRC
        ${CMAKE_SOURCE_DIR}/common/visual/gl/blend_function_neon.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/adjust_color_neon.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/colormap_neon.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/colorfill_neon.cpp
        ${CMAKE_SOURCE_DIR}/common/visual/gl/pixelformat_neon.cpp
    )
endif()

add_executable(${SIMD_TEST_TARGET} ${SIMD_TEST_SRC})

target_include_directories(${SIMD_TEST_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/common/environ
    ${CMAKE_SOURCE_DIR}/common/tjs2
    ${CMAKE_SOURCE_DIR}/common/base
    ${CMAKE_SOURCE_DIR}/common/extension
    ${CMAKE_SOURCE_DIR}/common/utils
    ${CMAKE_SOURCE_DIR}/common/visual
    ${CMAKE_SOURCE_DIR}/common/visual/gl
    ${CMAKE_SOURCE_DIR}/common/visual/IA32
    ${CMAKE_SOURCE_DIR}/external
    # tjsCommHead.h は Windows ビルド時 targetver.h を引くので
    ${CMAKE_SOURCE_DIR}/win32/vcproj
    # blend_function.cpp は _WIN32 で DetectCPU.h を引く
    ${CMAKE_SOURCE_DIR}/win32/environ
)

target_compile_definitions(${SIMD_TEST_TARGET} PRIVATE
    UNICODE
    _UNICODE
    __GENERIC__
    TVP_DONT_AUTOLOAD_PROVINCE
    TVP_DONT_AUTOLOAD_MASK
    KRKRZ_STANDALONE_TEST
)

# Architecture flags as preproc defines so simd_parity_test.cpp can gate
# SSE2/AVX2 sections (x86 only) and NEON section (ARM only) appropriately.
if (KRKRZ_TARGET_X86)
    target_compile_definitions(${SIMD_TEST_TARGET} PRIVATE KRKRZ_TEST_HAS_X86)
endif()
if (KRKRZ_TARGET_ARM)
    target_compile_definitions(${SIMD_TEST_TARGET} PRIVATE KRKRZ_TEST_HAS_NEON)
endif()

# ----------------------------------------------------------------------------
# 拡張命令の per-file コンパイルフラグ (sources.cmake と同条件)
# ----------------------------------------------------------------------------
if (KRKRZ_TARGET_X86)
    if (MSVC)
        set(_stest_avx2_flags "/arch:AVX2")
        set(_stest_ssse3_flags "")
    else()
        set(_stest_avx2_flags "-mavx2;-mfma")
        set(_stest_ssse3_flags "-mssse3")
    endif()

    foreach(_f
            ${CMAKE_SOURCE_DIR}/common/visual/gl/blend_function_avx2.cpp
            ${CMAKE_SOURCE_DIR}/common/visual/gl/colormap_avx2.cpp
            ${CMAKE_SOURCE_DIR}/common/visual/gl/colorfill_avx2.cpp
            ${CMAKE_SOURCE_DIR}/common/visual/gl/x86simdutilAVX2.cpp)
        set_source_files_properties(${_f}
            PROPERTIES COMPILE_OPTIONS "${_stest_avx2_flags}")
    endforeach()

    if (_stest_ssse3_flags)
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/common/visual/gl/blend_function_sse2.cpp
            ${CMAKE_SOURCE_DIR}/common/visual/gl/pixelformat_sse2.cpp
            PROPERTIES COMPILE_OPTIONS "${_stest_ssse3_flags}")
    endif()
endif()

# ----------------------------------------------------------------------------
# CTest 登録
# ----------------------------------------------------------------------------
add_test(NAME simd_parity COMMAND ${SIMD_TEST_TARGET})


# ============================================================================
# Phase SB1: sound SIMD パリティテスト
#
# simd_parity_test と同じ発想で、common/sound/ の SSE/NEON を C リファレンス
# と突き合わせる。float 演算なので byte-exact ではなく atol + rtol の複合
# トレランスで比較する。依存は SDL3/vcpkg/OpenGL なしでスタンドアロン。
# ============================================================================

set(SOUND_TEST_TARGET krkrz_sound_parity_test)

set(SOUND_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/sound_parity_test.cpp
    ${CMAKE_SOURCE_DIR}/common/sound/MathAlgorithms.cpp
    ${CMAKE_SOURCE_DIR}/common/sound/RealFFT.cpp
)

if (KRKRZ_TARGET_X86)
    list(APPEND SOUND_TEST_SRC
        ${CMAKE_SOURCE_DIR}/common/sound/MathAlgorithms_SSE.cpp
        ${CMAKE_SOURCE_DIR}/common/sound/RealFFT_SSE.cpp
        ${CMAKE_SOURCE_DIR}/common/sound/xmmlib.cpp
    )
endif()

if (KRKRZ_TARGET_ARM)
    list(APPEND SOUND_TEST_SRC
        ${CMAKE_SOURCE_DIR}/common/sound/MathAlgorithms_NEON.cpp
        ${CMAKE_SOURCE_DIR}/common/sound/RealFFT_NEON.cpp
    )
endif()

add_executable(${SOUND_TEST_TARGET} ${SOUND_TEST_SRC})

target_include_directories(${SOUND_TEST_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/common/environ
    ${CMAKE_SOURCE_DIR}/common/tjs2
    ${CMAKE_SOURCE_DIR}/common/base
    ${CMAKE_SOURCE_DIR}/common/extension
    ${CMAKE_SOURCE_DIR}/common/utils
    ${CMAKE_SOURCE_DIR}/common/sound
    ${CMAKE_SOURCE_DIR}/common/visual
    ${CMAKE_SOURCE_DIR}/common/visual/IA32
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/win32/vcproj
)

target_compile_definitions(${SOUND_TEST_TARGET} PRIVATE
    UNICODE
    _UNICODE
    __GENERIC__
    TVP_DONT_AUTOLOAD_PROVINCE
    TVP_DONT_AUTOLOAD_MASK
)

add_test(NAME sound_parity COMMAND ${SOUND_TEST_TARGET})
