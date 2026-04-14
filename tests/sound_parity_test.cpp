// ----------------------------------------------------------------------------
// sound_parity_test.cpp — sound SIMD パリティテスト
//
// common/sound/ の SSE 実装が C リファレンス実装と一致するかを、スタンドアロン
// に (SDL3 / vcpkg / 描画 / 音響デバイス依存なし) 検証するテスト。
//
// graphics 側の simd_parity_test と違い、float 演算の丸め差が不可避なので
// byte-exact ではなく **相対誤差 + 絶対誤差の複合トレランス** で比較する。
//
// 対象関数:
//   - rdft / rdft_sse               : Ooura Real DFT forward/inverse
//   - DeinterleaveApplyingWindow    : mono/stereo
//   - InterleaveOverlappingWindow   : mono/stereo (accumulator 系、dest 共有)
//
// Phase SB2 で NEON 版を追加するとき、`#ifdef KRKRZ_SOUND_TEST_HAS_NEON`
// ブロックを増やして run_neon() を呼び出すだけで済むように構成。
// ----------------------------------------------------------------------------

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <random>
#include <vector>

#include "tjsCommHead.h"
#include "MathAlgorithms.h"
#include "RealFFT.h"

// ---- Tolerance 定義 --------------------------------------------------------
// |ref - test| <= atol + rtol * max(|ref|, |test|)
struct Tolerance {
    float atol;
    float rtol;
};

// ---- 基本ユーティリティ ----------------------------------------------------
namespace {

struct Result {
    int pass = 0;
    int fail = 0;
    int skip = 0;
};

static Result g_result;

// seeded PRNG so results are reproducible across runs/sections
static std::mt19937& rng() {
    static std::mt19937 eng(0xA1FAE115u);
    return eng;
}

static void fill_random(float* buf, size_t n, float lo = -1.0f, float hi = 1.0f) {
    std::uniform_real_distribution<float> dist(lo, hi);
    auto& e = rng();
    for (size_t i = 0; i < n; ++i) buf[i] = dist(e);
}

static void fill_window(float* win, size_t n) {
    // Vorbis I 窓関数 (PhaseVocoderDSP.cpp::Process と同じ形)
    const double pi = 3.14159265358979323846;
    for (size_t i = 0; i < n; ++i) {
        double x = ((double)i + 0.5) / (double)n;
        double s = std::sin(pi * x);
        double w = std::sin(pi * 0.5 * s * s);
        win[i] = (float)w;
    }
}

// 相対+絶対トレランス比較。max mismatch と位置も返す。
struct Diff {
    float max_abs;     // |a - b| の最大値
    float max_rel;     // |a - b| / max(|a|, |b|, atol) の最大値
    int   first_idx;   // tolerance を超えた最初の位置 (-1 なら全合格)
    float first_ref;
    float first_test;
};

static Diff compare(const float* ref, const float* test, size_t n, Tolerance tol) {
    Diff d{0.0f, 0.0f, -1, 0.0f, 0.0f};
    for (size_t i = 0; i < n; ++i) {
        float a = ref[i];
        float b = test[i];
        float abs_diff = std::fabs(a - b);
        if (abs_diff > d.max_abs) d.max_abs = abs_diff;

        float denom = std::fmax(std::fmax(std::fabs(a), std::fabs(b)), 1e-12f);
        float rel = abs_diff / denom;
        if (rel > d.max_rel) d.max_rel = rel;

        float bound = tol.atol + tol.rtol * std::fmax(std::fabs(a), std::fabs(b));
        if (abs_diff > bound && d.first_idx < 0) {
            d.first_idx = (int)i;
            d.first_ref = a;
            d.first_test = b;
        }
    }
    return d;
}

static void report_pass(const char* name, const Diff& d) {
    std::printf("  pass: %-48s  max_abs=%.3e  max_rel=%.3e\n",
                name, d.max_abs, d.max_rel);
    g_result.pass++;
}

static void report_fail(const char* name, const Diff& d, Tolerance tol) {
    std::printf("  FAIL: %-48s  max_abs=%.3e  max_rel=%.3e  "
                "(atol=%.1e rtol=%.1e)\n"
                "         first mismatch @ %d: ref=%.7e  test=%.7e  diff=%.3e\n",
                name, d.max_abs, d.max_rel, tol.atol, tol.rtol,
                d.first_idx, d.first_ref, d.first_test,
                std::fabs(d.first_ref - d.first_test));
    g_result.fail++;
}

static void check(const char* name, const float* ref, const float* test,
                  size_t n, Tolerance tol) {
    Diff d = compare(ref, test, n, tol);
    if (d.first_idx < 0) report_pass(name, d);
    else                 report_fail(name, d, tol);
}

} // namespace

// ----------------------------------------------------------------------------
// rdft テスト
// ----------------------------------------------------------------------------
#if defined(TVP_SOUND_HAS_X86_SIMD) || defined(TVP_SOUND_HAS_ARM_SIMD)
using RdftFn = void (*)(int, int, float * __restrict,
                        int * __restrict, float * __restrict);

static void test_rdft_impl(const char* variant, RdftFn fn, int n) {
    // 入力を固定シードで生成
    std::vector<float> input(n);
    fill_random(input.data(), n, -1.0f, 1.0f);

    // Ooura の work 領域サイズ: ip = 2 + sqrt(n/4) ints, w = n/2 floats
    int ip_size = 2 + (int)std::sqrt((double)(n / 4)) + 1;
    std::vector<int>   ip_ref(ip_size, 0);
    std::vector<int>   ip_test(ip_size, 0);
    std::vector<float> w_ref(n / 2, 0.0f);
    std::vector<float> w_test(n / 2, 0.0f);

    // Forward
    {
        std::vector<float> a_ref = input;
        std::vector<float> a_test = input;
        rdft(n, 1, a_ref.data(), ip_ref.data(), w_ref.data());
        fn(n, 1, a_test.data(), ip_test.data(), w_test.data());
        char name[80];
        std::snprintf(name, sizeof(name), "%s rdft_fwd (n=%d)", variant, n);
        // FFT の誤差は O(log n) × machine epsilon、サイズ 2048 で rtol 1e-5 は十分
        check(name, a_ref.data(), a_test.data(), n, {1e-5f, 1e-5f});
    }

    // Inverse (forward を通した後の結果を使う方が現実的だが、ここでは
    // 独立入力で比較する)
    {
        std::vector<float> a_ref = input;
        std::vector<float> a_test = input;
        rdft(n, -1, a_ref.data(), ip_ref.data(), w_ref.data());
        fn(n, -1, a_test.data(), ip_test.data(), w_test.data());
        char name[80];
        std::snprintf(name, sizeof(name), "%s rdft_inv (n=%d)", variant, n);
        check(name, a_ref.data(), a_test.data(), n, {1e-5f, 1e-5f});
    }
}
#endif

// ----------------------------------------------------------------------------
// Deinterleave/Interleave 共通ロジック (x86 + NEON で使い回す)
// ----------------------------------------------------------------------------
namespace {

using DeinterleaveFn = void (*)(float * __restrict dest[],
                                const float * __restrict src,
                                float * __restrict win,
                                int numch, size_t destofs, size_t len);
using InterleaveFn   = void (*)(float * __restrict dest,
                                const float * __restrict const * __restrict src,
                                float * __restrict win,
                                int numch, size_t srcofs, size_t len);

static void test_deinterleave_impl(const char* variant, DeinterleaveFn fn,
                                   int numch, size_t len) {
    std::vector<float> src(len * numch);
    std::vector<float> win(len);
    fill_random(src.data(), src.size(), -1.0f, 1.0f);
    fill_window(win.data(), len);

    std::vector<std::vector<float>> dest_ref(numch, std::vector<float>(len, 0.0f));
    std::vector<std::vector<float>> dest_test(numch, std::vector<float>(len, 0.0f));
    std::vector<float*> dest_ref_ptrs(numch);
    std::vector<float*> dest_test_ptrs(numch);
    for (int c = 0; c < numch; ++c) {
        dest_ref_ptrs[c] = dest_ref[c].data();
        dest_test_ptrs[c] = dest_test[c].data();
    }

    DeinterleaveApplyingWindow(dest_ref_ptrs.data(), src.data(),
                               win.data(), numch, 0, len);
    fn(dest_test_ptrs.data(), src.data(), win.data(), numch, 0, len);

    for (int c = 0; c < numch; ++c) {
        char name[80];
        std::snprintf(name, sizeof(name),
                      "%s Deinterleave (ch=%d/%d, len=%zu)",
                      variant, c, numch, len);
        check(name, dest_ref[c].data(), dest_test[c].data(), len, {1e-7f, 1e-6f});
    }
}

static void test_interleave_impl(const char* variant, InterleaveFn fn,
                                 int numch, size_t len) {
    std::vector<std::vector<float>> src_data(numch, std::vector<float>(len));
    std::vector<const float*> src_ptrs(numch);
    for (int c = 0; c < numch; ++c) {
        fill_random(src_data[c].data(), len, -1.0f, 1.0f);
        src_ptrs[c] = src_data[c].data();
    }
    std::vector<float> win(len);
    fill_window(win.data(), len);

    std::vector<float> dest_seed(len * numch);
    fill_random(dest_seed.data(), dest_seed.size(), -0.5f, 0.5f);

    std::vector<float> dest_ref = dest_seed;
    std::vector<float> dest_test = dest_seed;

    InterleaveOverlappingWindow(dest_ref.data(), src_ptrs.data(),
                                win.data(), numch, 0, len);
    fn(dest_test.data(), src_ptrs.data(), win.data(), numch, 0, len);

    char name[80];
    std::snprintf(name, sizeof(name),
                  "%s Interleave (numch=%d, len=%zu)",
                  variant, numch, len);
    check(name, dest_ref.data(), dest_test.data(), len * numch, {1e-7f, 1e-6f});
}

} // namespace

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main() {
    std::printf("=== sound_parity_test ===\n");

#if defined(TVP_SOUND_HAS_X86_SIMD)
    std::printf("-- x86 (SSE vs C reference) --\n");

    for (int n : {256, 512, 1024, 2048}) {
        test_rdft_impl("SSE", rdft_sse, n);
    }

    for (size_t len : {128, 256, 512, 1024}) {
        test_deinterleave_impl("SSE", DeinterleaveApplyingWindow_sse, 1, len);
        test_deinterleave_impl("SSE", DeinterleaveApplyingWindow_sse, 2, len);
    }
    for (size_t len : {128, 256, 512, 1024}) {
        test_interleave_impl("SSE", InterleaveOverlappingWindow_sse, 1, len);
        test_interleave_impl("SSE", InterleaveOverlappingWindow_sse, 2, len);
    }
#endif

#if defined(TVP_SOUND_HAS_ARM_SIMD)
    std::printf("-- ARM NEON vs C reference --\n");

    // Phase SB2-a: MathAlgorithms_NEON の deinterleave / interleave。
    // Phase SB2-b: RealFFT_NEON (rdft_neon) を追加。
    // ProcessCore_neon は Phase SB2-c で追加予定。
    for (int n : {256, 512, 1024, 2048}) {
        test_rdft_impl("NEON", rdft_neon, n);
    }

    for (size_t len : {128, 256, 512, 1024}) {
        test_deinterleave_impl("NEON", DeinterleaveApplyingWindow_neon, 1, len);
        test_deinterleave_impl("NEON", DeinterleaveApplyingWindow_neon, 2, len);
    }
    for (size_t len : {128, 256, 512, 1024}) {
        test_interleave_impl("NEON", InterleaveOverlappingWindow_neon, 1, len);
        test_interleave_impl("NEON", InterleaveOverlappingWindow_neon, 2, len);
    }
#endif

#if !defined(TVP_SOUND_HAS_X86_SIMD) && !defined(TVP_SOUND_HAS_ARM_SIMD)
    std::printf("-- no SIMD sections available for this architecture --\n");
    g_result.skip++;
#endif

    std::printf("\n=== summary: pass=%d fail=%d skip=%d ===\n",
                g_result.pass, g_result.fail, g_result.skip);
    return g_result.fail == 0 ? 0 : 1;
}
