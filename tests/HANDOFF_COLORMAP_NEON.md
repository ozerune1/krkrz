# Phase 2 colormap: NEON 実装の ARM Linux 引き継ぎ

Linux ARM (aarch64) 環境で Phase 2 colormap の NEON 実装 (C5-C6) を
書き切るための引き継ぎメモ。Windows + Linux x86_64 側では C1-C4 まで
完了済み。

## 現状

- 最新コミット: `1d83ae0e`
- ブランチ: master
- Windows x64 harness: SSE2 327/327 + AVX2 327/327 全 pass
- Linux x86_64 harness: 327/327 全 pass (ユーザー確認済)
- Linux ARM64 harness: **未実行** (直前の NEON commit は `db34e99f`
  (adjust_color) まで。本 handoff はその次の colormap NEON 実装)

## Phase 2 colormap 全体の作業状態

| コミット | 内容 | 状態 |
|---|---|---|
| `c2fac7e2` | C1: harness + SSE2 非 HDA alpha fix | ✅ 完了 |
| `bfb9bce6` | C3: AVX2 ColorMap base + _o (4 関数) | ✅ 完了 |
| `1d83ae0e` | C4: AVX2 ColorMap _a / _ao / _d (5 関数) | ✅ 完了 |
| **C5** | **NEON ColorMap base + _o (4 関数)** | **本作業対象** |
| **C6** | **NEON ColorMap _a / _ao / _d (5 関数)** | **本作業対象** |

9 関数の SSE2 wired 一覧:
- `TVPApplyColorMap` / `TVPApplyColorMap65` (base)
- `TVPApplyColorMap_o` / `TVPApplyColorMap65_o` (base + opa)
- `TVPApplyColorMap_a` / `TVPApplyColorMap65_a` (additive)
- `TVPApplyColorMap_ao` / `TVPApplyColorMap65_ao` (additive + opa)
- `TVPApplyColorMap65_d` (destructive, 65 のみ)

## 参考にするファイル

### 設計の出発点 (必読)

- `common/visual/gl/colormap_sse2.cpp` (778 行)
  - NEON 実装はこの SSE2 のアルゴリズムを 128bit NEON でほぼ直訳する
  - 4 pixel/iter が自然なサイズ (NEON = 128bit で SSE2 と同じ幅)
  - C1 で入れた非 HDA alpha 修正 (`rgb_mask_` / `& 0x00ffffff`) も移植する

- `common/visual/gl/colormap_avx2.cpp` (新規追加、570 行ぐらい)
  - AVX2 版の参考実装。per-channel 計算式、mc_ 構築 (特に _a functor の
    `0x100 | broadcasti128` トリック)、_d の scalar table gather 等の
    ロジックは同じなので流用可能。
  - ただし AVX2 は 8 pixel で 2 lane に分かれる構造で、NEON は 4 pixel
    単一レジスタなのでそこは違う。

### NEON のスタイル参考

- `common/visual/gl/blend_function_neon.cpp` (既存 NEON 本体)
  - 8 pixel/iter で `vld4_u8` deinterleave して B/G/R/A を個別レジスタで
    扱う pattern (blend ファミリ)。これは「channel-major」レイアウト。
  - 一方で ColorMap の SSE2 は「pixel-major」(1 pixel = 4ch を連続 16bit
    に unpack) で書かれている。
  - NEON で ColorMap を書くときは **どちらを選ぶか** を最初に決める:
    (a) 4 pixel を `vld1q_u32` → `vmovl_u8` で pixel-major にして
        SSE2 直訳する (コード量少、可読性高)
    (b) 8 pixel を `vld4_u8` で channel-major にして既存 NEON スタイルに
        揃える (コード量多、CPU 利用効率高)
  - 個人的な推奨は **(a) 4 pixel pixel-major**。理由:
    - SSE2/AVX2 と 1:1 対応するのでデバッグが楽
    - ColorMap は text rendering が主用途で per-pixel 処理が中心
    - 既存 NEON コードも PsBlend は pixel-major 系の作りになっている
    - 16 byte align は `vld1q_u32` で自然
  - (b) を選ぶ場合は blend_function_neon.cpp の vld4_u8 スタイル参照。

- `common/visual/gl/adjust_color_neon.cpp` (Phase 2 第一弾)
  - スカラフォールバックと opaque/transparent fast path 込みの最小構成
  - colormap の opaque fast path (`s == topaque` なら `color_` を store)
    は SSE2 と同じ構造で書ける

## NEON で必要な intrinsics (4 pixel pixel-major 前提)

```c
#include <arm_neon.h>

// ロード
uint32x4_t md = vld1q_u32(dest);      // 4 pixel
uint32_t s4 = *(const uint32_t*)src;  // 4 src byte をまとめて u32 で
// scalar で個別に読んでも OK: src[0], src[1], src[2], src[3]

// 16bit 展開
uint8x16_t md_u8 = vreinterpretq_u8_u32(md);
uint16x8_t md_lo = vmovl_u8(vget_low_u8(md_u8));   // pixels 0,1 の 8 u16
uint16x8_t md_hi = vmovl_u8(vget_high_u8(md_u8));  // pixels 2,3 の 8 u16

// color 展開 (mc_ の構築、コンストラクタで 1 回だけ)
uint32x4_t color_v = vdupq_n_u32(color);
uint16x8_t mc      = vmovl_u8(vget_low_u8(vreinterpretq_u8_u32(color_v)));
// mc は (0A, 0R, 0G, 0B, 0A, 0R, 0G, 0B) = 2 pixel 分の broadcast

// 基本演算
uint16x8_t diff  = vsubq_u16(mc, md_lo);         // c - d (符号付き意味、u16 wrap)
uint16x8_t prod  = vmulq_u16(diff, mo_lo);       // * opa
int16x8_t  shr6  = vshrq_n_s16(vreinterpretq_s16_u16(prod), 6);  // srai_epi16 相当
int16x8_t  sum   = vaddq_s16(vreinterpretq_s16_u16(md_lo), shr6);

// パック + 非 HDA alpha mask
uint8x8_t  pack_lo = vqmovun_s16(sum);           // packus_epi16 相当
// 2 pixel 分 (8 byte) → さらに hi とまとめて 16 byte にする
uint8x16_t packed  = vcombine_u8(pack_lo, pack_hi);
uint32x4_t result  = vreinterpretq_u32_u8(packed);
result = vandq_u32(result, vdupq_n_u32(0x00ffffff));  // alpha=0
vst1q_u32(dest, result);
```

### tshift=8 (256 variant) の byte-wrap trick

SSE2 / AVX2 の 256 variant は `srli_epi16` + `add_epi8` の byte-wrap
trick で signed 意味を保持していた (colormap_sse2.cpp / colormap_avx2.cpp
のコメント参照)。NEON でも同じパターン:

```c
// mc = (c - d), prod = mc * s (16bit wrap で負数を保持)
uint16x8_t shr8  = vshrq_n_u16(prod, 8);          // srli_epi16 相当 (unsigned)
uint8x16_t md_bytes = vreinterpretq_u8_u16(md_lo);
uint8x16_t mc_bytes = vreinterpretq_u8_u16(shr8);
uint8x16_t sum_bytes = vaddq_u8(md_bytes, mc_bytes);  // add_epi8 相当 (wrap)
// packus 相当で low byte を取り出し
```

この trick が NEON でも働くことは数学的には保証されているが、実機で
確認する必要あり。

### _d functor の table gather

SSE2/AVX2 と同じく scalar で 4 pixel 分 `TVPNegativeMulTable65` /
`TVPOpacityOnOpacityTable65` を引いてから NEON レジスタに組み立てる。
NEON には直接の gather 命令がないので scalar loop が妥当。

```c
uint32_t addr0 = (src[0] << 8) | (d_arr[0] >> 24);
// ... addr1-3
int16_t o0 = TVPOpacityOnOpacityTable65[addr0];
// ... o1-3
uint16x8_t mo_lo = {o0, o0, o0, o0, o1, o1, o1, o1};
uint16x8_t mo_hi = {o2, o2, o2, o2, o3, o3, o3, o3};
```

## 実装手順 (推奨)

### ステップ 1: `common/visual/gl/colormap_neon.cpp` を新規作成

構造:
```cpp
#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "neonutil.h"
#include <arm_neon.h>

extern "C" {
extern unsigned char TVPNegativeMulTable65[65*256];
extern unsigned char TVPOpacityOnOpacityTable65[65*256];
}

namespace {

// base functor tshift=6 / tshift=8
// ...

// straight / _o wrapper
// ...

// _a functor
// ...

// _d functor
// ...

// apply_color_map_branch_func_neon / apply_color_map_func_neon helpers
// ...

} // anonymous namespace

// エクスポート 9 関数
void TVPApplyColorMap65_neon_c(...) { ... }
// etc
```

### ステップ 2: wiring

- `sources.cmake` の `KRKRZ_SRC_ARM_GRAPHICS_SIMD` に追加:
  ```
  common/visual/gl/colormap_neon.cpp
  ```
- `tests/CMakeLists.txt` の ARM ブランチに同じく追加
- `common/visual/gl/blend_function_neon.cpp::TVPGL_NEON_Init` に extern
  宣言と 9 関数の割り当て追加 (adjust_color の時と同じパターン)

### ステップ 3: ビルド + harness 実行

```sh
make test
# あるいは
cmake --build build/arm64-linux --config Release --target krkrz_simd_parity_test
ctest --test-dir build/arm64-linux -C Release -R simd_parity -V
```

### ステップ 4: failure triage

NEON section で新規 9 関数が pass することを確認。想定される問題:
- base 非 HDA alpha が 0 になっているか (C1 の SSE2 修正と同じ方針)
- tshift=8 の byte-wrap trick が NEON でも動いているか
- _a functor の mc_ 構築で alpha 位置に 0x100 が入っているか
- _d functor の scalar table gather / alpha 上書きが正しいか

tolerance は harness 側で既に設定されている:
- 基本 byte-exact
- _a/_ao: tol_rgb=2
- _d: tol_alpha=1

NEON 側でも同じ許容で良いはず (SSE2 / AVX2 と同じ tolerance で 9 関数
全てが pass することを目標にする)。

### ステップ 5: commit + memory 更新

- C5 (base + _o): 1 コミット
- C6 (_a / _ao / _d): 1 コミット
- memory `project_simd_rollout.md` の Phase 2 colormap セクションに
  完了記録を追加

## ハマりどころ

### cpu_detect で NEON フラグが立つか

現在 `common/visual/cpu_detect.cpp::TVPInitCPUFeatures()` は aarch64 で
`TVP_CPU_HAS_ARM_NEON | TVP_CPU_HAS_ARM64_ASIMD` を無条件で立てる。
NEON section の harness は C1 の先行実装で `TVPGL_NEON_Init()` を直接
呼ぶ形になっているので、追加 wiring は TVPGL_NEON_Init だけ見ればよい。

### 16 byte align

SSE2 の apply_color_map_branch_func_sse2 は 16 byte align 調整を
している。NEON の `vld1q_u32` は unaligned load もサポートする
(aarch64 では align 不要) ので、もっとシンプルな loop でよい。既存
blend_function_neon.cpp の blend_func_neon と同じスタイル:

```cpp
tjs_int rem = (len >> 2) << 2;  // 4 pixel 単位
tjs_uint32 *limit = dest + rem;
while (dest < limit) {
    // ... 4 pixel 処理
    dest += 4; src += 4;
}
while (dest < /* orig limit */) {
    // scalar fallback
}
```

### tshift=8 の byte-wrap

NEON の `vaddq_u8` は unsigned wrap add なので SSE2/AVX2 の `add_epi8`
と 1:1 対応する。`vshrq_n_u16(prod, 8)` は unsigned shift right で
`_mm_srli_epi16(_, 8)` と一致。trick は同じはず。

### _d の scalar gather

per-pixel 4 回の `TVPOpacityOnOpacityTable65[addr]` 参照 + 4 回の
`TVPNegativeMulTable65[addr]` 参照が必要。
`vgetq_lane_u32` か `vst1q_u32` でレジスタから配列に書き出してから
scalar 参照、set で NEON レジスタに戻す流れ。

## 完了判定

Linux ARM64 で `make test` が:
- NEON section: 243 → **252 / 252 passed, 0 failed** (adjust_color 1 +
  colormap 9 = 10 の加算を想定。ただし 65_d 込みで 9 関数なので正確には
  252 が想定値)
- Windows/Linux x86_64 で回帰なし (327/327 維持)

が確認できれば完了。memory `handoff_colormap_neon.md` を CLOSED 化して
本ドキュメントも履歴として残す。
