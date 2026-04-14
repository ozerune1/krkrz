# ResampleImage NEON 実装: 保留 (deferred)

2026-04-11 の Phase 2 pass 時点で **意図的に保留**。本ドキュメントは
将来再開する際のコンテキスト引き継ぎ。

## なぜ保留したか

### 規模

`common/visual/gl/ResampleImageSSE2.cpp` は **1210 行**。
`ResampleImageAVX2.cpp` は **1272 行**。他の Phase 2 カテゴリ
(colormap 778 行、colorfill 431 行、boxblur 620 行) と比べて
2〜3 倍の規模。

### 内部構成

1 ファイルに 3 種のアルゴリズム × (Fix/float) + thread worker が全部
入っている:

- **Bicubic resample**: `TVPBicubicResampleSSE2Fix` / `TVPBicubicResampleSSE2`
  シャープネス可変、4x4 kernel、float or fixed-point 演算
- **AreaAvg resample**: `TVPAreaAvgResampleSSE2Fix` / `TVPAreaAvgResampleSSE2`
  ダウンスケール専用の area-weighted averaging
- **Weight-based resample**: `TVPWeightResampleSSE2Fix` / `TVPWeightResampleSSE2`
  汎用、template functor (Lanczos2/3, Spline16/36, Gaussian,
  BilinearWeight, BlackmanSincWeight 等)

各アルゴリズムの SIMD 本体は `ResamplerSSE2FixFunc` / `ResamplerSSE2Func`
(thread worker) に 600 行超で詰まっている。SSE2 / AVX2 で下記のような
混在コードを使う:

- `__m128` / `__m128i` 混在 (float/int 演算の交互)
- 水平合計 (`m128_hsum_sse1_ps` 等の独自 util) で kernel 積和を reduce
- fixed-point は 15bit 精度固定小数 (`M128_PS_FIXED15`) を使った
  浮動→整数変換
- `aligned_allocator<>` + SSE2 の 16byte align を前提にした weight 配列
- template functor 経由で重み関数を埋め込む

Bicubic / AreaAvg / Weight の 3 本 × Fix/float 2 本 × SIMD worker
(fix/float) 2 本 = 概ね 6 つの実装塊 + dispatch 関数 +
init + 共通 helper。

### 依存

`#include` 先が他カテゴリより重い:
- `LayerBitmapIntf.h` / `LayerBitmapImpl.h` — bitmap 本体型 (thread-local
  な描画コンテキストを参照)
- `ThreadIntf.h` — `TJS_USERENTRY` thread worker entry macro
- `WeightFunctorSSE.h` — 専用 weight functor
- `ResampleImageInternal.h` — axis param templates
- `aligned_allocator` — SSE2 16byte align 前提
- `x86simdutil.h` — m128 水平加算等の独自 helper

NEON 化するには上記に加えて:
- `WeightFunctorNEON.h` 相当を新規作成 (NEON 向けの weight 計算)
- `aligned_allocator` は NEON 128bit でも align 条件同じなので流用可
- `x86simdutil` の `m128_hsum_sse1_ps` 相当 (vaddvq_f32 は aarch64 専用)

### harness で検証できない

`tests/simd_parity_test.cpp:50-51` で `TVPInitializeResampleSSE2/AVX2`
を**空 stub 化**している:

```cpp
void TVPInitializeResampleSSE2() {}
void TVPInitializeResampleAVX2() {}
```

これは parity test を SDL3/vcpkg/Thread/LayerBitmap 依存から切り離す
ための意図的な除外。ResampleImage NEON を書いても harness では
pass/fail を確認できず、**本体ビルド + 実画像での visual 比較**
(元画像 → 4 つのバックエンド (C ref / SSE2 / AVX2 / NEON) でリサイズ
→ PSNR/SSIM 比較 or 目視) が必要になる。harness にも独立の比較手法
を入れるか、別プロジェクトで verify する必要がある。

### dispatcher 変更が必要

`common/visual/gl/ResampleImage.cpp:689-718` が現在の dispatcher:

```cpp
void TVPResampleImage(...) {
    ...
    if (TVPCPUType & TVP_CPU_HAS_AVX2 && ...) {
        TVPResampleImageAVX2(...);
    } else if (TVPCPUType & TVP_CPU_HAS_SSE2 && ...) {
        TVPResampleImageSSE2(...);
    } else {
        // C ref テンプレート dispatch (TVPBicubicResample / _AreaAvg / _Weight)
    }
}
```

NEON 対応には `else if (TVPCPUType & TVP_CPU_HAS_ARM_NEON || ...)` 分岐
を追加し、`TVPResampleImageNEON(...)` を呼ぶ必要がある。現状 ARM では
C ref テンプレート dispatch に落ちていて、これはこれで動作する。
**機能的に NEON 化が必須ではない** (動くがやや遅い)。

## 現状の ARM 動作

- `TVPCPUType` に `TVP_CPU_HAS_ARM_NEON | TVP_CPU_HAS_ARM64_ASIMD` が
  立つ (cpu_detect.cpp)
- しかし `TVPResampleImage` の dispatcher は x86 bit しか見ていない
  ので、ARM では C ref template dispatch に落ちる
- `TVPBicubicResample` / `TVPAreaAvgResample` / `TVPWeightResample<T>`
  の C reference (ResampleImage.cpp) がそのまま走る
- 実用上は「StretchBlt の品質設定に応じて C reference が動く」状態。
  速度は SIMD 最適化版より遅いが、Phase 2 の他カテゴリと違い機能
  自体は完結している

## 今後再開する場合の進め方

### 前提条件

- ResampleImage 専用の検証手段を先に用意する (harness 拡張 or 独立
  visual test)
- 本体ビルド + 実画像でのテストができる Linux ARM64 環境
- C ref / SSE2 / NEON で **同一入力**に対する出力の PSNR 比較
  ツール (PSNR 40dB 以上なら実用上問題なし、とか判断基準を先に決める)

### 推奨実装順序

1. **verify 基盤作り** (1 セッション)
   - harness か別プロジェクトで「同一画像を 4 種エンジンでリサイズ
     して出力を byte / PSNR 比較」する仕組みを作る
   - SSE2 vs C ref の現時点での差を先に観測 (既知の丸めズレが
     どこにあるか確認)
   - 可能なら tolerance policy を先に決める

2. **AreaAvg NEON 実装** (1 セッション)
   - 3 アルゴリズム中で最もシンプル (weighted average の degenerate case)
   - fixed-point 版だけで十分な場合が多い
   - SSE2 との比較で tolerance check

3. **Bicubic NEON 実装** (1 セッション)
   - 4x4 kernel で weight 計算あり、中規模
   - sharpness パラメータ周りの float math が注意点

4. **Weight-based NEON 実装** (2 セッション)
   - template functor まわりの設計が難所 (WeightFunctorNEON.h 新規作成)
   - 全 functor (Bilinear, Lanczos2/3, Spline16/36, Gaussian,
     BlackmanSinc) を NEON に下ろす

5. **dispatcher + wiring** (0.5 セッション)
   - ResampleImage.cpp の dispatcher に ARM NEON 分岐追加
   - blend_function_neon.cpp の TVPGL_NEON_Init で
     `TVPInitializeResampleNEON()` 呼び出し
   - sources.cmake / tests/CMakeLists.txt の KRKRZ_SRC_ARM_GRAPHICS_SIMD
     に追加

### 想定コード規模

- `ResampleImageNEON.cpp` 本体: 1000〜1200 行 (SSE2/AVX2 と同規模)
- `WeightFunctorNEON.h` 新規: 200〜300 行
- `ResampleImage.cpp` dispatcher 修正: ~10 行
- 検証ツール: 300〜500 行 (別セッションで先に作る)

合計で **4〜5 セッション** 相当の作業量 (Phase 2 の他カテゴリが
1〜2 セッションで完結していたのに対して顕著に大きい)。

## 参考ファイル

- `common/visual/gl/ResampleImageSSE2.cpp` (1210 行) — アルゴリズム直訳元
- `common/visual/gl/ResampleImageAVX2.cpp` (1272 行) — 256bit 拡張版、
  NEON 128bit で直訳するなら SSE2 側を見る方が近い
- `common/visual/gl/ResampleImage.cpp` — dispatcher と C ref template 本体
- `common/visual/gl/ResampleImageInternal.h` — 共有型定義
- `common/visual/gl/WeightFunctorSSE.h` — SSE weight functor (NEON 版の
  参考)
- `common/visual/gl/x86simdutil.h` — 水平加算等の SIMD util (NEON 対応
  が要る)

## 関連

- `tests/HANDOFF_COLORFILL_NEON.md` (CLOSED 2026-04-11) — より小さな
  カテゴリの NEON 実装 handoff パターン、参考に
- memory `project_simd_rollout.md` — Phase 2 全体の rollout plan
