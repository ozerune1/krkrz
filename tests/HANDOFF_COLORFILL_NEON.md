# Phase 2 colorfill: NEON 実装 (D3) の ARM Linux 引き継ぎ

colormap NEON (C5-C6) と同じフローで、colorfill の NEON 版 7 関数を
ARM Linux 環境で実装・検証するための引き継ぎメモ。

## 現状

- 最新コミット: `e6d0eac2` (AVX2 Phase 2 D2: colorfill 7 関数)
- ブランチ: master
- Windows x64 harness: SSE2 343/343 + AVX2 343/343 両セクション pass
- Linux x86_64 harness: **未実行** (前回 Linux x86_64 確認後に D1/D2 が
  積まれたので、NEON 実装と併せて回帰確認したい)
- Linux ARM64 harness: **未実行** (前回は colormap NEON までで 268/268 pass)

## 作業状態

| コミット | 内容 | 状態 |
|---|---|---|
| `5ebcff1f` | D1: harness 拡張 + SSE2 divergence 観察 | ✅ 完了 |
| `e6d0eac2` | D2: AVX2 colorfill 7 関数 | ✅ 完了 |
| **D3** | **NEON colorfill 7 関数** | **本作業対象** |

## 対象 7 関数 (SSE2 / AVX2 で既に実装済み)

### 単純 fill 系 (単純コピー、byte-exact 期待)
- `TVPFillARGB` — 単色 ARGB 書き込み (32 pixel/iter unrolled store)
- `TVPFillARGB_NC` — 同上、non-temporal store (_mm_stream_si128 / NEON は該当無、通常 store で OK)
- `TVPFillColor` — HDA: `(d & 0xff000000) | (color & 0x00ffffff)` (RGB 上書き、alpha 保持)
- `TVPFillMask` — HDA: `(d & 0x00ffffff) | (mask << 24)` (alpha 上書き、RGB 保持)

### 単色 blend 系
- `TVPConstColorAlphaBlend` — `(d * (255-opa) + color * opa) >> 8`、HDA 保持 (byte-exact)
- `TVPConstColorAlphaBlend_d` — TVPOpacityOnOpacityTable/NegativeMulTable で dst alpha 更新 (tol_alpha=1)
- `TVPConstColorAlphaBlend_a` — premul additive alpha (tol_alpha=1 / tol_rgb=2、opa=255 は skip)

## 参考にするファイル

### 設計の出発点 (必読)

- `common/visual/gl/colorfill_sse2.cpp` (431 行)
  - NEON はこの SSE2 の計算式を NEON 128bit でほぼ直訳する
  - 4 pixel/iter が自然 (PsBlend / colormap NEON と同じ pixel-major スタイル推奨)

- `common/visual/gl/colorfill_avx2.cpp` (新規、約 450 行)
  - AVX2 版。`avx2_const_alpha_fill_blend_{functor,d_functor,a_functor}` の
    コンストラクタと operator() のロジックを流用可能
  - `_a` functor の `opa += opa>>7` / `msa` 構築 / `mc_ = color*opa + msa`
    の組み立ては SSE2/AVX2 と同じに

- `common/visual/gl/colormap_neon.cpp` (commit 98049300)
  - colormap NEON の成功パターン。ファイル構造・functor スタイル・wiring
    をそのまま流用できる

- `common/visual/gl/adjust_color_neon.cpp` (Phase 2 第一弾)
  - 単独関数 + scalar fallback + NEON fast path のミニマル構成

## 実装手順

### ステップ 1: `common/visual/gl/colorfill_neon.cpp` を新規作成

推奨構成:
```cpp
#include "tjsCommHead.h"
#include "tvpgl.h"
#include "tvpgl_ia32_intf.h"
#include "neonutil.h"
#include <arm_neon.h>

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[256*256];
extern unsigned char TVPNegativeMulTable[256*256];
}

// TVPFillARGB / _NC: vst1q_u32 の 4 発アンロールで 16 pixel/iter
// TVPFillColor / TVPFillMask: 定数合成、vandq_u32 + vorrq_u32
// TVPConstColorAlphaBlend: 4 pixel unpack → blend → pack
// TVPConstColorAlphaBlend_d: scalar gather (TVPOpacityOnOpacityTable / Negative)
// TVPConstColorAlphaBlend_a: mc_ 組み立てを neon で、operator() で d*mo -> d-dmd+mc
```

### ステップ 2: 計算式の NEON 対応表

SSE2 → NEON 対応:
| SSE2 | NEON (128bit, pixel-major) |
|---|---|
| `_mm_set1_epi32(v)` | `vdupq_n_u32(v)` |
| `_mm_setzero_si128()` | `vdupq_n_u8(0)` / `vdupq_n_u16(0)` |
| `_mm_load_si128` | `vld1q_u32` (align 要求緩い) |
| `_mm_store_si128` | `vst1q_u32` |
| `_mm_stream_si128` | 通常の `vst1q_u32` で良い (NEON に NT store 相当は無いか扱い微妙) |
| `_mm_and_si128` | `vandq_u32` |
| `_mm_or_si128` | `vorrq_u32` |
| `_mm_unpacklo_epi8(v, 0)` | `vmovl_u8(vget_low_u8(v_as_u8))` |
| `_mm_unpackhi_epi8(v, 0)` | `vmovl_u8(vget_high_u8(v_as_u8))` |
| `_mm_mullo_epi16(a, b)` | `vmulq_u16(a, b)` |
| `_mm_srli_epi16(v, n)` | `vshrq_n_u16(v, n)` |
| `_mm_slli_epi16(v, n)` | `vshlq_n_u16(v, n)` |
| `_mm_adds_epu16(a, b)` | `vqaddq_u16(a, b)` |
| `_mm_add_epi16(a, b)` | `vaddq_u16(a, b)` |
| `_mm_sub_epi16(a, b)` | `vsubq_u16(a, b)` |
| `_mm_add_epi8(a, b)` | `vaddq_u8(a, b)` (byte-wrap trick) |
| `_mm_packus_epi16(lo, hi)` | `vcombine_u8(vqmovun_s16(lo_as_s16), vqmovun_s16(hi_as_s16))` |

pixel-major 4 pixel 展開:
```c
uint32x4_t md = vld1q_u32(dest);                              // 4 pixels
uint8x16_t md_u8 = vreinterpretq_u8_u32(md);
uint16x8_t md_lo = vmovl_u8(vget_low_u8(md_u8));              // pixels 0,1
uint16x8_t md_hi = vmovl_u8(vget_high_u8(md_u8));             // pixels 2,3
// ... blend ...
uint8x8_t  out_lo = vqmovun_s16(vreinterpretq_s16_u16(md_lo));
uint8x8_t  out_hi = vqmovun_s16(vreinterpretq_s16_u16(md_hi));
uint8x16_t packed = vcombine_u8(out_lo, out_hi);
vst1q_u32(dest, vreinterpretq_u32_u8(packed));
```

### ステップ 3: wiring

- `sources.cmake` の `KRKRZ_SRC_ARM_GRAPHICS_SIMD` に追加:
  ```
  common/visual/gl/colorfill_neon.cpp
  ```
- `tests/CMakeLists.txt` の ARM ブランチにも同じく追加
- `blend_function_neon.cpp::TVPGL_NEON_Init` に extern 宣言 + 7 関数割り当て

### ステップ 4: ビルド + harness

```sh
# まず Linux x86_64 で回帰確認
make test   # or: cmake --build build/x64-linux --target krkrz_simd_parity_test && ctest ...

# 次に Linux ARM64 で NEON 検証
make test  # arm64-linux preset
ctest --test-dir build/arm64-linux -C Release -R simd_parity -V
```

### ステップ 5: 期待される結果

- **Linux x86_64**: SSE2 343/343 + AVX2 343/343 維持 (前回 327/327 から
  colormap C5-C6 と colorfill D1/D2 で +16 関数)
- **Linux ARM64**: NEON section 268 → **275 / 275 passed, 0 failed**
  (+7 関数 = FillARGB_NC / FillMask / ConstColorAlphaBlend × 5 opa +
  _d × 5 + _a × 4 + fill の既存 2 も NEON に差し替えで +2 = 実際には
  計算が複雑、数値は実行して確認)

## ハマりどころ

### `TVPFillARGB_NC` の non-temporal store

NEON には `vst1q_u32` の non-temporal 版は無い。`__builtin_nontemporal_store`
は Clang 拡張として使えるが、`vst1q_u32` で普通に書いて良いはず。
non-temporal の目的は cache pollution 回避なので、ARM では通常の store
でも問題は小さい。**単に `vst1q_u32` で通せば OK。**

### `TVPConstColorAlphaBlend_a` の opa=255 edge case

SSE2 / AVX2 と同じく `opa += opa>>7` で opa=256 になると `(opa<<24)` で
signed int32 overflow を起こす。harness 側で既に opa=255 は skip 設定
(`if (opa == 255) printf skip...`) になっているので、NEON 実装でも
素直に SSE2 と同じ計算式で書けば同じ "bug" を再現して OK。

### `vshrq_n_u16` / `vshlq_n_u16` は immediate 要求

shift amount は compile-time 定数でなければならない。template 化するか
個別に展開する。

### `TVPConstColorAlphaBlend_d` の scalar gather

per-pixel 4 回の `TVPOpacityOnOpacityTable[addr]` + `TVPNegativeMulTable[addr]`
参照。NEON レジスタから scalar 配列にいったん store (`vst1q_u32` か
`vgetq_lane_u32 × 4`) → scalar loop で lookup → `vld1q_u32` で戻す、の
pattern。AVX2 版 (colorfill_avx2.cpp の `avx2_const_alpha_fill_blend_d_functor`)
の operator() と同じ構造。

## 完了判定

1. `common/visual/gl/colorfill_neon.cpp` が新規作成されている
2. sources.cmake / tests/CMakeLists.txt / blend_function_neon.cpp に
   wiring が入っている
3. Linux ARM64 で NEON section 275/275 (または想定値) pass
4. Linux x86_64 回帰なし (343/343 維持)
5. Windows x64 回帰なし (343/343 維持)

完了したら:
- コミット: 1 個または適宜 family 分割 (D3)
- memory `handoff_colorfill_neon.md` を CLOSED 化
- `project_simd_rollout.md` の Phase 2 colorfill セクションに完了記録

## 関連ドキュメント

- `tests/HANDOFF_COLORMAP_NEON.md` — colormap NEON 実装 handoff (CLOSED)
- `tests/HANDOFF_NEON_LINUX.md` — 初期 NEON 検証 handoff (CLOSED)
- memory `project_simd_rollout.md` — Phase 2 全体ロードマップ
