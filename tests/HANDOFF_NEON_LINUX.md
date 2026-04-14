# Phase 1 NEON 実装の Linux 検証 handoff

このドキュメントは Phase 1 NEON 実装一式を Windows MSVC でブラインドに書き
終えた直後の状態 (commit `e18c4d2f`) で残したものです。Linux ARM/ARM64 環境
で本格的な検証を開始する人向けの引き継ぎメモです。

## 状況

- 最新コミット: `e18c4d2f` (2026-04-11)
- Windows x64 での harness 結果: SSE2 302/302 + AVX2 302/302 全 pass
- NEON コードはコンパイル/実行ともに **未検証**。ARM 環境での初実行で問題が
  出る可能性が高い。

## 直近の Phase 1 NEON 関連コミット

| commit | 内容 |
|---|---|
| `2f76ffba` | NEON: AdditiveAlphaBlend ファミリの functor を実装 + wiring 有効化 |
| `cd2aa018` | NEON: 加算 / 減算 / 乗算 / Lighten / Darken / Screen の素朴なブレンド functor を追加 |
| `617c9b0b` | NEON: PsBlend ファミリ第一弾 (alpha/add/sub/mul/screen) |
| `8943d1df` | NEON: PsBlend ファミリ第二弾 (lighten/darken/diff/exclusion) |
| `53df4302` | NEON: PsBlend ファミリ完了 (overlay/hardlight/softlight/colordodge/colorburn/colordodge5/diff5) |
| `e18c4d2f` | SIMD parity test を ARM Linux で NEON 検証可能にする (CMakeLists + test cpp gating) |

## 検証手順

### 1. Linux x86_64 で 302/302 を再現 (回帰チェック)

NEON 着手前に Linux 側でも Phase B / Phase 1 AVX2 PsBlend が問題なく
ビルド・実行できることを確認:

```sh
cd <repo>
cmake --preset x64-linux        # preset がなければ build/x64-linux に手動
cmake --build build/x64-linux --target krkrz_simd_parity_test
ctest --test-dir build/x64-linux -R simd_parity --output-on-failure
```

期待結果: `OK: all SIMD variants match C reference` で SSE2 302/302 +
AVX2 302/302。

### 2. ARM ターゲットでビルド + 実行

```sh
cmake --preset arm64-linux      # preset がなければ追加 (KRKRZ_TARGET_ARM=ON が立つこと)
cmake --build build/arm64-linux --target krkrz_simd_parity_test
ctest --test-dir build/arm64-linux -R simd_parity --output-on-failure
```

CMake 側で `KRKRZ_TARGET_ARM` が ON になると自動で:

- `common/visual/gl/blend_function_neon.cpp` がビルド対象になる
- `tests/CMakeLists.txt` 経由で `KRKRZ_TEST_HAS_NEON` define が test target に
  渡される
- `tests/simd_parity_test.cpp` の `#ifdef KRKRZ_TEST_HAS_NEON` パスが有効化
  される
- harness 内で `TVPCPUType` に `ARM_NEON | ARM64_ASIMD` を立てて
  `TVPGL_NEON_Init()` を呼ぶ → snapshot → run_suite

期待される出力構造:

```
krkrz SIMD parity test
======================
TVPCPUType = 0x00000003
  NEON  : yes

[NEON vs C reference]
  pass TVPAlphaBlend
  ...
  -> NN / 302 passed, MM failed
======================
```

302/302 にはまずならないと思っていてください。以下の想定問題ガイドを参考に
fail を 1 つずつ潰す。

## 想定される問題と対処方針

### A. ビルドエラー

NEON intrinsics の typo, immediate constant チェック等。確認すべきファイル:

- `common/visual/gl/blend_functor_neon.h` (commit `2f76ffba`, `cd2aa018` で
  追加した部分)
- `common/visual/gl/blend_ps_functor_neon.h` (新規ファイル、`617c9b0b` 以降
  3 コミットで作成)

`#error "NEON intrinsics not available with the soft-float ABI"` が出たら
`-mfpu=neon` (32bit ARM) または `-mfloat-abi=hard` を CMake 側で渡す必要が
あるかも。ARM64 では何もせず通るはず。

### B. NEON 数値結果が C ref と違う

harness が `pass` か `FAIL` かを出してくれる。FAIL の場合の対処:

#### 旧来の `neon_alpha_blend` 系 (TVPAlphaBlend / _o / HDA / _d / _a)

これらは Phase B 着手前から NEON ファイルにあった既存コードで誰も触って
いない。Phase B 後の C ref と byte-exact 一致するかは未確認。FAIL したら
SSE2/AVX2 と同じく `(d * (255-a) + s * a) >> 8` の rounding 違いで ±1 ズレ
ている可能性。harness の test_lintrans_tol / test_univtrans の tolerance を
参考に、適切な family に tolerance を入れて吸収する (`tol_alpha=0,
tol_rgb=1` 程度)。

#### `neon_premul_alpha_blend_*` (commit `2f76ffba`)

Phase B/AVX2 と同じ `(d * (255-sa)) >> 8 + s` 掛け算ベースで書いた。
byte-exact で通るはず。FAIL したら `vshrn_n_u16(vmull_u8(...), 8)` の
rounding を `vrshrn_n_u16` (rounding shift) と取り違えていないか確認。

#### 基本 blend (Add/Sub/Mul/Lighten/Darken/Screen) — commit `cd2aa018`

byte-exact のはず。Mul/Screen の C ref も `(d*s) >> 8` truncation。FAIL
した場合は alpha レーンが clear されてるか (これら 4 family は結果 alpha=0)。

#### PsBlend ファミリ全 16 種 — commits `617c9b0b` / `8943d1df` / `53df4302`

PsBlend 専用トレランス (`tol_alpha=-1` 完全無視 + `tol_rgb=2`、ColorDodge5
のみ `tol_rgb=8`) が harness 側で適用されている。NEON は SSE2 と違って
byte-exact 8bit 精度で書いたので tol 内に余裕で収まるはず。FAIL する場合は
`neon_ps_alpha_blend_base<pre_func, HDA>` のロジック (pre_func 適用 →
3チャネル alpha blend → HDA template arg で alpha 上書き or clear) が
pre_func の意味と整合してるか確認。

#### Table-based PsBlend (SoftLight / ColorDodge / ColorBurn / ColorDodge5)

`neon_ps_table_blend<TTable, HDA>` テンプレートで実装。8 pixel 分を
`alignas(16) uint8_t bD[8], gD[8], rD[8], bS[8], gS[8], rS[8]` のスタック
バッファに store → ループで `TTable::TABLE[s][d]` 参照 → load し直す。
FAIL したら `vst1_u8` / `vld1_u8` の方向が正しいか、`ps_*_table::TABLE`
シンボルが `blend_functor_c.h` 経由で見えているか確認。

#### LinTransAlphaBlend_a / UnivTransBlend / Gamma 系

これらは NEON 未対応 (deferred)。harness は `skip (same as C reference)`
で表示する (NEON が override しないので ref==test で skip 判定)。
気にしなくてよい。

## 修正方針

Phase B / Phase 1 で確立した方針:

1. **C リファレンスを真とする**。SIMD 側を C ref に揃える。
2. SSE2 PsBlend のように **byte-exact 化が構造的に難しい** ものは harness 側に
   トレランスを入れる (`memory: project_psblend_tolerance.md`)。
3. NEON は SSE2 と違って byte-exact 化が現実的なので、まず NEON 側のコード
   を修正する。tolerance を緩めるのは最後の手段。
4. 修正が大きくなる場合は family 単位で commit を分ける (Phase B / Phase 1
   PsBlend と同じスタイル)。

## まだ未実装で残っているもの (Linux 検証スコープ外)

これらは Phase 1 完了の対象から外して deferred:

- AVX2 LinTrans / UnivTrans / Gamma (gather / table lookup bound)
- NEON LinTrans / UnivTrans / Gamma (同上)
- mul/lighten/darken/screen の `_o` バリアント (NEON; opa 乗算 + 既存 blend
  の 2 段組)
- `TVPLinTransAlphaBlend_a` の SSE2 修正 (`sse2_alpha_blend_a_functor` が C ref
  `TVPAddAlphaBlend_a_d` と semantic divergence、TODO 扱い)

## 関連ドキュメント

- `tests/simd_parity_test.cpp`: harness 本体
- `tests/CMakeLists.txt`: ビルド設定 (KRKRZ_TARGET_X86 / ARM 分岐)
- `common/visual/gl/blend_functor_neon.h`: NEON 基本 blend ファンクタ
- `common/visual/gl/blend_ps_functor_neon.h`: NEON PsBlend ファンクタ
- `common/visual/gl/blend_function_neon.cpp`: NEON wrapper + init 関数
- `common/visual/cpu_detect.{h,cpp}`: portable な CPU 機能検出エントリ
  `TVPInitCPUFeatures()`。本体ビルド (Win32/Generic) と test harness で
  共通に使う (commit `a21d053d` で導入)。

## 完了したらやること

1. Linux NEON で 302/302 が通ったら, このファイルに完了日と最終コミットを
   追記
2. `git log --oneline` で `Linux NEON 検証完了` のコミットを 1 本入れて
   それを記録

---

## 完了記録

- **完了日**: 2026-04-10
- **検証環境**: NanoPi-R6S (Linux 6.1.57 aarch64, GCC 12.x, native aarch64-linux-gnu)
- **最終コミット**: `b4f4a60d NEON: Linux ARM64 SIMD parity 242/242 達成`
- **結果**: `ctest -C Debug -R simd_parity --output-on-failure` → **242/242 passed, 0 failed**
  (x86 環境の 302 との差は x86 専用関数分で、NEON 対応範囲は全て pass)

### 実際に遭遇した問題と対処

ビルドが通る前の整備 (想定問題 A の系統):

1. **objcopy target 不整合**
   `build/arm64-linux/CMakeCache.txt` に x86 時代の `OBJFORMAT=elf64-x86-64`
   が焼き付いており、`-B aarch64` と組み合わさって `invalid bfd target`。
   → `CMakeLists.txt` で `OBJFORMAT` / `OBJARCH` の `CACHE STRING` をやめ、
   configure 毎に `KRKRZ_TARGET_ARM` から再計算するよう変更。

2. **`TVP_CPU_HAS_ARM_NEON` / `TVP_CPU_HAS_ARM64_ASIMD` 未定義**
   `blend_function_neon.cpp` では `#if defined(NN_NINTENDO_SDK)` ブロック内
   でのみ定義されており Linux からは見えない。`simd_parity_test.cpp` も
   `tvpgl_ia32_intf.h` を `#ifdef _WIN32` 限定で include していた。
   → `tvpgl_ia32_intf.h` に ARM flag を追加 + `TVP_GL_IA32_FUNC_*_DECL` の
   非 Windows 版 (`__cdecl` 抜き) を追加。両 cpp から include を有効化。

3. **`neon_const_alpha_blend_a_functor` const member 未初期化**
   `const struct neon_premul_alpha_blend_a_functor blend_;` を値初期化
   していなかったため GCC が deleted default ctor を警告→エラー。
   → init list に `, blend_{}` を追加。

NEON ファンクタ本体のバグ (想定問題 B の系統):

4. **Darken / Lighten / Screen の alpha レーン誤クリア** (3 family 消化)
   C ref (`darken_blend_functor` 等) は 32bit 一括 min/max/screen なので
   alpha も min(da,sa) / max(da,sa) / 0xff になる。NEON は `A_VEC(md) =
   vdup_n_u8(0)` で 0 にしていた。
   → alpha レーンも該当演算を適用するよう修正。Screen は式自体が
   `d + s - (d*s)>>8` 形だと ±1 ズレが出るので `~((~d)*(~s)>>8)` 形に
   書き直し、alpha は `vdup_n_u8(0xff)` 固定。

5. **PsExclusionBlend vector の narrow バグ** (10 fail 消化)
   `vshrn_n_u16(vmull_u8(a,b), 7)` は u16→u8 に narrow するため、
   `2*d*s/256` が最大 508 なのに 0..255 に truncate されて破綻。
   → `vshrq_n_u16` で u16 のまま保持し、最後に `vqmovn_u16(vqsubq_u16(...))`
   で saturate narrow。

6. **AddBlend_o の alpha 誤加算** (4 fail 消化)
   C ref `add_blend_func` は `s` を opa でスケールする際 alpha バイトを
   マスクで落としてから全チャネル sat add するので、結果 alpha は
   `sat(da + 0) = da`。NEON は src alpha もスケール加算していた。
   → scalar / vector とも dest alpha そのまま保持に修正。

7. **Legacy alpha_blend 系の rounding 差** (23 fail 消化、最大の山)
   NEON の `(s*a + d*(255-a)) >> 8` 形式は C ref の
   `d + ((s-d)*a) >> 8` と ±1 ズレる (SSE2/AVX2 は後者で書かれていて
   byte-exact パスしていた)。
   → SSE2/AVX2 と同じく符号付き 16bit で書き直した。ただし最初の試行で
   `vmulq_s16(diff, a16)` を使ったところ `255*255 = 65025` が s16
   (-32768..32767) を越えて wrap し、14 個失敗 (中間値に `00` や `ff`
   が残る症状) が発生。
   → `vmull_s16` (s16 * s16 → s32) で widen してから `vshrq_n_s32(·, 8)`
   で算術右シフト、最後に `vmovn_s32` で s16 に戻して `vqmovun_s16` で
   u8 に sat。これで **0 failed に到達**。

### 学び / 次回のための memo

- **narrow 系 intrinsic の上限**: `vshrn_n_u16(prod, n)` は u8 に narrow
  するので、演算の中間値が 255 を越える形 (2*d*s など) では必ず u16 の
  まま保持すること。narrow は最後の sat 段階だけに使う。
- **signed s16 乗算の上限**: 符号付き算術で書く場合、`u8 * u8 = 65025`
  は s16 を越える。`vmull_s16` の widen 乗算を使うのが定石。
- **C ref の alpha レーンの扱いは family ごとにまちまち**:
  - `darken_blend_functor` / `lighten_blend_functor` → 32bit 一括 min/max
    なので alpha も min(da,sa) / max(da,sa)
  - `screen_blend_functor` → 内部で bit 24-31 を経由するが最終的に 0xff 固定
  - `add_blend_func(d,s,a)` → s の alpha バイトを途中でマスクで落とすので
    結果 alpha = dest alpha
  - `alpha_blend_func(d,s,a)` → 3 チャネルしか触らないので alpha = 0
  - `mul_blend_functor` → 結果 alpha = 0
  これらは NEON 側で各 functor ごとに明示的に揃えないと byte-exact に
  ならない。
