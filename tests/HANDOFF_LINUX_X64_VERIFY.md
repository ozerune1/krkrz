# Linux x86_64 SIMD parity 検証 handoff

**Status: CLOSED 2026-04-11**
Linux x86_64 (GCC 15) で `krkrz_simd_parity_test` の SSE2/AVX2 両セクションが
302/302 で通ることを確認。`TVPCPUType = 0xdfdbf110` (SSE2/SSSE3/AVX2 yes)。
Windows 専用構文を多数引きずっていたので、GCC/Clang で通るよう以下を修正:

- `simd_def_x86x64.h`, `x86simdutil.h`: `<intrin.h>` を `_MSC_VER` ガード下に
  移し、非 MSVC では `<x86intrin.h>` を使う。
- `simd_def_x86x64.h`: `__m128i.m128i_u32[i]` / `.m128i_i32` / `.m128i_u8`
  および `__m256i.m256i_u32` / `.m256i_i32` は MSVC 専用ユニオン拡張。
  portable な `M128I_U32(v,i)` / `M128I_I32` / `M128I_U8` / `M256I_U32` /
  `M256I_I32` マクロ (GCC/Clang は aligned スタック一時にストア、MSVC は
  従来のメンバアクセスに展開) を追加し、全 call site を置換。
- `*_sse2.cpp` 内の `(unsigned)ptr & 0xF` 等の 32bit ポインタキャストを
  `(uintptr_t)ptr` に変更 (x86_64 で precision 警告→エラー)。
- `blend_ps_functor_sse2.h` / `blend_ps_functor_avx2.h`: `const blend_func
  blend_;` を初期化子リストに追加 (`-fpermissive` 下でもエラー)。
- `colormap_sse2.cpp`: テンプレート派生クラスの two-phase lookup 対応で
  `zero_` を `this->zero_` に、`apply_color_map_branch_func_sse2` の
  `int topaque` テンプレートパラメータを `tjs_uint32 topaque` に変更
  (`0xffffffff` の narrowing 回避)。
- `univtrans_sse2.cpp`: `not` は C++ の予約語 (alternative token) なので
  `not_mask` にリネーム。
- `pixelformat_sse2.cpp`, `blend_function_sse2.cpp` の ssse3 `shuffle_epi8`
  用 `mask` 初期化: MSVC 専用ユニオン書き込みを `_mm_setr_epi8(...)` に
  置き換え。
- `detect_cpu.cpp`: GCC の `<cpuid.h>` は `__cpuid` をマクロで定義し
  `__cpuidex` を関数で定義している。以前の asm 実装は highest=uninit を
  return しており、引数数も不一致。`__cpuid` / `__cpuid_count` builtin に
  ラッパを書き直し、既存の call site は `#undef __cpuid` してマクロで
  `krkrz_cpuid` / `krkrz_cpuidex` へ forwarding。
- `sources.cmake` / `tests/CMakeLists.txt`: `pixelformat_sse2.cpp` にも
  `-mssse3` per-file フラグを追加 (`_mm_shuffle_epi8` を含むため)。

いずれも pixel-exact は壊していない (AVX2 ディスパッチの実機テストで
fail 0)。

### 2026-04-11 追補: Phase 2 AVX2 ファイル追加時の per-file flag 漏れ

Phase 2 で追加された `colormap_avx2.cpp` / `colorfill_avx2.cpp` が
`sources.cmake` の `_krkrz_avx2_files` に登録されておらず、本体 (krkrz)
ビルドで `-mavx2 -mfma` が付かず GCC 15 で `always_inline` intrinsics が
target option mismatch で落ちていた。テストハーネス側
(`tests/CMakeLists.txt`) には登録済みだったので parity test は通るが
本体が通らない状態だった。`_krkrz_avx2_files` に両ファイルを追記して解消。
parity test は引き続き 302/302 pass。

---

このドキュメントは Linux x86_64 環境で `krkrz_simd_parity_test` を初めて
回す作業のための引き継ぎメモです。Linux ARM64 (NEON) 検証は別ドキュメント
`HANDOFF_NEON_LINUX.md` を参照のこと (2026-04-11 に完了済み)。

## 背景

これまで Linux x86_64 では:

- **SSE2/AVX2 が一切 wired されていなかった**。`generic/base/SysInitImpl.cpp`
  の旧 CPU 検出経路は ARM 無条件 NEON しか面倒を見ておらず、Linux/macOS
  x86_64 ビルドは TVPCPUType=0 のまま起動し pure-C reference で動いていた。
- 2026-04-11 の commit `a21d053d` で `common/visual/cpu_detect.cpp` の
  `TVPInitCPUFeatures()` に CPU 検出を統一。これにより Linux/macOS x86_64
  でも初めて cpuid + `__builtin_cpu_supports("avx2")` で AVX OS-support を
  判定して SSE2/AVX2 init が呼ばれるようになった。
- harness (`tests/simd_parity_test.cpp::main`) も同じ `TVPInitCPUFeatures()`
  を経由するように統一されている。

ただし Windows 上で書かれた変更で、Linux x86_64 での実行確認は **未実施**。
これがこの handoff の対象。

## 直近の関連コミット

| commit | 内容 |
|---|---|
| `a21d053d` | CPU 機能検出を portable な共通エントリ TVPInitCPUFeatures() に統一 |
| `f8a9e431` | NEON: NN_NINTENDO_SDK 専用の TVPCPUType 個別定義を撤去 |
| `c39d2863` | sources.cmake / DetectCPU.cpp の死にコード整理 |
| `a8f3b96d` | ResampleImage: AVX2/SSE2 dispatch を _WIN32 限定から x86 全 OS 共通に開放 |
| `200bb3ad` | docs/make: SIMD parity テストの実行手順を README + make test に追加 |

## 検証手順

### 1. リポジトリ取得

```sh
git clone <repo> krkrz
cd krkrz
git submodule update --init
git log --oneline -5  # 200bb3ad またはそれ以降が見えれば OK
```

`VCPKG_ROOT` を設定。

### 2. ビルド + テスト実行

`x64-linux` preset を使う:

```sh
make prebuild           # = cmake --preset x64-linux
make build              # 念のため本体ビルドも回しておく (オプション)
make test               # = krkrz_simd_parity_test ビルド + ctest -R simd_parity
```

または直接:

```sh
cmake --preset x64-linux
cmake --build build/x64-linux --config Release --target krkrz_simd_parity_test
ctest --test-dir build/x64-linux -C Release -R simd_parity --output-on-failure
```

### 3. 中身を確認 (重要)

`ctest --output-on-failure` は **失敗時にしか stdout を出さない** ので、
"1/1 Passed" だけだと `TVPCPUType` の値や per-section の pass 数が見えない。
中身を見るには `-V` を付けるか実行ファイルを直接叩く:

```sh
ctest --test-dir build/x64-linux -C Release -R simd_parity -V
# あるいは
./build/x64-linux/Release/krkrz_simd_parity_test
```

### 4. 確認すべき出力

期待される出力:

```
krkrz SIMD parity test
======================
TVPCPUType = 0x000000XX   # SSE2(0x04) | MMX(0x10) | AVX(0x...) | AVX2(0x...) 等
                          # (実際のビット定義は common/visual/cpu_detect.h を参照)
  SSE2  : yes
  AVX2  : yes (CPU が対応していれば)

[SSE2 vs C reference]
  pass TVPAlphaBlend
  pass TVPAlphaBlend_HDA
  ...
  -> 302 / 302 passed, 0 failed

[AVX2 vs C reference]
  pass TVPAlphaBlend
  ...
  -> 302 / 302 passed, 0 failed

OK: all SIMD variants match C reference
======================
```

### 5. 確認ポイント

#### (A) `TVPInitCPUFeatures()` が AVX2 を正しく検出するか

ホスト CPU が AVX2 対応 (Haswell 以降) なら `TVPCPUType` に AVX2 ビットが
立つことを確認。立たない場合は `common/visual/cpu_detect.cpp` の GCC/Clang
ブロック (`__builtin_cpu_init()` + `__builtin_cpu_supports("avx2")`) が
正しく評価されているか確認する。AVX2 のみ落ちて AVX が立つなら OS support
チェックが原因の可能性。

ホストが AVX2 非対応なら `[AVX2] skipped (CPU does not report AVX2)` が
出るのが正常で、これはバグではない。

#### (B) SSE2 セクションが 302/302 で通るか

Phase A/B (Windows 上で確定済み) と同じ結果が出るはず。**もし FAIL する
場合、それは Linux x86_64 固有の問題** (例えば `__cdecl` マクロの扱い、
`_mm_set1_epi32` の引数型、`alignas` まわり) が混入している可能性。

#### (C) AVX2 セクションが 302/302 で通るか

同上。AVX2 だけ落ちる場合は per-file `-mavx2 -mfma` フラグが GCC/Clang で
効いてない可能性 (sources.cmake の `KRKRZ_SRC_X86_GRAPHICS_SIMD` を確認)。

### 6. 完了したら

- memory `project_simd_rollout.md` の「CPU 機能検出経路の統一」セクションに
  Linux x86_64 検証完了 (commit XXXXXXX) を追記
- このファイル (`tests/HANDOFF_LINUX_X64_VERIFY.md`) は CLOSED 扱いにする
  (削除でも、ステータス追記でも可)

## 想定されるトラブルと対処

### ビルドエラー

- `__builtin_cpu_init` / `__builtin_cpu_supports` が見つからない
  → 古い GCC (4.x 以前) で起こる。`common/visual/cpu_detect.cpp` の
    GCC バージョンガードを追加する必要があるかもしれない。
- `-mavx2 -mfma` で `*_avx2.cpp` のコンパイルが落ちる
  → Clang のバージョンによる。`-mavx2 -mfma -mfma4` でなく単に `-mavx2`
    で十分なはず。sources.cmake の per-file flag を疑う。
- `_mm256_extracti128_si256` 等の immediate constant エラー
  → MSVC 拡張 (`m256i_u32[i]` 直接アクセス) を Clang 診断が拒否してる
    可能性。MSVC ビルド前提のコードがあれば書き直す。

### 実行時の数値ズレ

Phase A/B / Phase 1 で確立した triage 方針:

1. C リファレンスを真とする。SIMD 側を C ref に揃える。
2. PsBlend ファミリは harness 側 tolerance policy で吸収済み
   (`tol_alpha=-1, tol_rgb=2`、ColorDodge5 のみ `tol_rgb=8`)
3. それ以外で ±1 以上ズレる場合は SSE2/AVX2 ファンクタを修正する

## 関連ドキュメント

- `tests/HANDOFF_NEON_LINUX.md`: ARM64 NEON 検証 handoff (CLOSED 2026-04-11)
- `README.md` "SIMDパリティテスト" セクション: 一般的な実行方法
