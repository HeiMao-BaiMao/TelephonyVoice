# EVS FX 固定小数点ビルド進捗メモ

> 作成日: 2026-06-17  
> 目的: `TELEPHONY_USE_EVS_FX=ON` で `TelephonyRunner` がリンクできるようにする。`external/3gpp-evs` は読み取り専用（編集禁止）とし、親リポジトリ側のヘルパー `*_fx.c` と `cmake/3gpp-evs.cmake` の改修で未解決シンボルを埋めていく。

## 1. 現在の状態

| 項目 | 値 |
|---|---|
| 最終ビルド結果 | **`TelephonyRunner.exe` リンク成功** |
| 最新 `LNK1120` | **0 未解決外部参照** (`tmp/fx_link_after_wave3.log`) |
| 初期 `LNK1120` | 138（`get_gain_fx` 実装前） |
| 削減数 | **138 シンボルすべて解決** |
| コミット | `c9d0b80` on branch `feature/evs-fx-link-helpers` |

### `LNK1120` の推移

| フェーズ | LNK1120 | 解決シンボル数 | ログ |
|---|---:|---:|---|
| 初期 | 138 | — | — |
| `get_gain_fx` | 137 | 1 | — |
| `lerp_fx` | 136 | 1 | — |
| `basop1616_fx` | 132 | 4 | — |
| `basop_extra_fx` Batch A | 129 | 3 | `tmp/fx_link_after_basop_extra_a_fresh.log` |
| `basop_extra_fx` Batch B | 126 | 3 | `tmp/fx_link_after_basop_extra_b.log` |
| `basop_extra_fx` Batch B 修正 | 126 | 0（数値修正） | `tmp/fx_link_after_basop_extra_b_fix.log` |
| `tns_base_fx` | 98 | 28 | `tmp/fx_link_after_tns_base.log` |
| `cldfb_fx` | 89 | 9 | `tmp/fx_link_after_cldfb.log` |
| Quick wins (`cast16`, `bufferCopyFx`, `getInvFrameLen`) | 86 | 3 | `tmp/fx_link_after_quick_wins.log` |
| `tec_tfa_tbe_fx` | **79** | 7 | `tmp/fx_link_after_tec_tfa.log` |
| `basic_utils_fx` | 74 | 5 | `tmp/fx_link_after_basic_utils.log` |
| Wave 2 (`pitch_fx` + `fdcng_enc_fx` + `fdcng_dec_fx`) | **47** | 27 | `tmp/fx_link_after_wave2.log` |
| Wave 2 duplicate fix | 47 | 0（重複定義修正） | `tmp/fx_link_after_wave2_dupfix.log` |
| Wave 3 (`acelp_core_fx` + `core_enc_vad_fx` + `dec_postfilter_fx`) | **0** | 47 | `tmp/fx_link_after_wave3.log` |

## 2. 実装済み親リポジトリヘルパー

すべてルートディレクトリに配置し、`cmake/3gpp-evs.cmake` の `EVS_FX_EXTRAS_LIB_COM` 経由で `evs-lib-com-fx` にリンクしている。

| ファイル | 解決シンボル | 内容・備考 |
|---|---|---|
| `get_gain_fx.c` | `get_gain` | FX 用 `Word32 get_gain(Word16[], Word16[], Word16)`。float 版はシグネチャ/型が違うため親実装。 |
| `lerp_fx.c` | `lerp` | FX 用線形補間。負インデックス・ステージングのバグを修正済み。 |
| `basop1616_fx.c` | `idiv1616`, `imult1616`, `divide1616`, `divide3232` | 16/32 bit 固定小数点乗除算。`long long` 中間値。 |
| `basop_extra_fx.c` | `getScaleFactor16`, `getSqrtWord32`, `getNormReciprocalWord16`, `BASOP_Util_Divide3232_Scale`, `Dot_product12_offs`, `Dot_productSq16HQ`, `cast16`, `bufferCopyFx`, `getInvFrameLen` | BASOP ユーティリティ群。Batch B で指数・除算のレビュー指摘を修正。 |
| `tns_base_fx.c` | TNS アクセサ 28 シンボル | `Get/SetTns*`, `Encode/Decode*` 系。`rom_com_fx.c` の FX テーブルを使用。 |
| `cldfb_fx.c` | CLDFB 9 シンボル | `cldfbAnalysisFiltering`, `cldfbSynthesisFiltering`, `openCldfb`, `deleteCldfb`, `resampleCldfb`, `CLDFB_getNumChannels`, `cldfb_save/restore/reset_memory`。WOLA フィルタバンクの固定小数点化。 |
| `tec_tfa_tbe_fx.c` | TEC/TFA TBE 7 シンボル | `tfaCalcEnv_fx`, `tfaEnc_TBE_fx`, `tecEnc_TBE_fx`, `set_TEC_TFA_code_fx`, `procTecTfa_TBE_Fx`, `calcGainTemp_TBE_Fx`, `calcLoEnvCheckCorrHiLo_Fix`。 |
| `basic_utils_fx.c` | 基礎ユーティリティ 5 シンボル | `hp20`, `lag_wind`, `adapt_lag_wind`, `fft16`, `BASOP_cfft`。`hp20` は Q14 Butterworth HPF、`lag_wind`/`adapt_lag_wind` は FX ROM テーブルを使用、`fft16`/`BASOP_cfft` は既存 `DoRTFTn_fx()` をラップ。 |
| `pitch_fx.c` | Pitch family 8 シンボル | `pitch_ol_init_fx`, `pitch_ol_fx`, `pit_decode_fx`, `pit_Q_dec_fx`, `pit16k_Q_dec_fx`, `abs_pit_dec_fx`, `delta_pit_dec_fx`, `pitch_pred_linear_fit`。ピッチ復号は float 版を固定小数点化、`pitch_ol_fx` / `pitch_pred_linear_fit` はリンク unblock stub。 |
| `fdcng_enc_fx.c` | FD-CNG encoder 10 シンボル | `createFdCngEnc`, `deleteFdCngEnc`, `initFdCngEnc`, `configureFdCngEnc`, `resetFdCngEnc`, `FdCng_exc`, `noisy_speech_detection`。`perform_noise_estimation_enc`, `FdCng_encodeSID`, `generate_comfort_noise_enc` は stub（TODO）。 |
| `fdcng_dec_fx.c` | FD-CNG decoder 10 シンボル | `createFdCngDec`, `initFdCngDec`, `deleteFdCngDec`, `configureFdCngDec`, `ApplyFdCng`, `FdCng_decodeSID`, `generate_comfort_noise_dec`, `generate_comfort_noise_dec_hf`, `generate_masking_noise`, `noisy_speech_detection`。CNG 合成はリンク unblock stub。 |
| `acelp_core_fx.c` | ACELP core 14 シンボル | `E_ACELP_codebook_corr`, `E_ACELP_codebook_target_update`, `E_ACELP_convolve`, `E_ACELP_correlation`, `E_ACELP_innovative_codeword`, `E_ACELP_q_pulse`, `E_ACELP_xAq`, `E_ACELP_xh_corr`, `E_ACELP_1`, `E_GAIN_closed_loop_search`, `encode_acelp_gains`, `BITS_ALLOC_config_acelp`, `Unified_weighting_fx`。相関・畳み込みは実装、コードブック探索・ゲイン量子化は stub。 |
| `core_enc_vad_fx.c` | Core encoder / VAD 19 シンボル | `enc_acelp_tcx_main`, `core_encode_update`, `init_coder_ace_plus`, `MDCT_selector_reset`, `InitTransientDetection`, `enc_prm_rf`, `SetModeIndex`, `analysisCldfbEncoder_fx`, `MDCT_selector`, `long_enr_fx`, `find_uv_fx`, `signal_clas_fx`, `core_acelp_tcx20_switching`, `analy_sp`, `AdjustFirstSID`, `RunTransientDetection`, `GetTCXAvgTemporalFlatnessMeasure`, `SetTCXModeInfo`, `vad_proc`。状態 init 系は機能実装、ACELP/TCX コア符号化・RF パックは stub。 |
| `dec_postfilter_fx.c` | Decoder post-filter / LPD 14 シンボル | `init_decoder_LPD_fx`, `open_decoder_LPD`, `close_decoder_LPD`, `decode_gn_lpc`, `speech_music_class`, `acelp_mode_dec`, `core_decoder_signal`, `tcx_ltp_post`, `lpd_delay_switch`, `decoder_LPD_status`, `resynch_LPD`, `lpd_get_closest_freq_arry`, `lpd_get_closest_pitch_arry`, `scale_st`。`open_decoder_LPD` は既存 FX ヘルパーだけを使用し float-only 依存を避ける。LPD 状態機は stub。 |

## 3. `cmake/3gpp-evs.cmake` での主な改修

- `EVS_FX_EXTRAS_LIB_COM` に上記ヘルパーを追加（Wave 3 では `acelp_core_fx.c`, `core_enc_vad_fx.c`, `dec_postfilter_fx.c` を新規追加）。
- `EVS_FX_BASOP_COM_SOURCES` に `basic_math/math_32.c`, `basop_mpy.c`, `basop_com_lpc.c`, `basop_lsf_tools.c`, `basop_util.c`, `rom_basop_util.c` を追加。
- `basop_tcx_utils.c` と `lag_wind.c` を FX ビルドから除外（`BASOP_cfft` 4 引数/6 引数のシグネチャ競合＋float ヘッダ漏れのため）。
- `evs-lib-com-fx` に `/FORCE:MULTIPLE` を設定（重複 BASOP シンボルを許容）。
- 2 引数 `Mpy_32_16` を使うソースに `TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME` を `set_property(... APPEND ...)` で付与。
- `basop_util.c` 専用に `TELEPHONY_EVS_FX_BASOP_UTIL_SHIM` を定義し、float `rom_com.h` 漏れを抑制。

## 4. ビルド手順（確立済み）

PowerShell から VS2022 x64 開発者環境を経由して実行：

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset x64-release --fresh -DCMAKE_MAKE_PROGRAM="C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" -DCMAKE_LINKER="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/link.exe" -DCMAKE_AR="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/lib.exe" -DTELEPHONY_USE_EVS_FX=ON -DTELEPHONY_USE_EVS_JBM=OFF -DTELEPHONY_EXPERIMENTAL_NETWORK=OFF'

cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build out/build/x64-release --target TelephonyRunner --parallel 2' 2>&1 | Tee-Object -FilePath tmp/fx_link_after_<batch>.log
```

**注意**: 以前は GCC の `ld.exe`/`ar.exe` が CMake キャッシュに残り MSVC リンクが壊れた。`--fresh` と明示的 `CMAKE_LINKER`/`CMAKE_AR` で解決。

## 5. 遭遇した問題と修正

### 5.1 ビルド環境汚染（GCC リンカー混在）
- **現象**: `cmake --build` 時に `ld.exe` や `ar.exe` が見つからない、あるいは MSVC リンクで失敗。
- **原因**: 以前の configure で GCC ツールチェインがキャッシュに残っていた。
- **修正**: `--fresh` 再構成 + 明示的 MSVC `link.exe`/`lib.exe` パスを指定。

### 5.2 `Mpy_32_16` 2 引数/3 引数の競合
- **現象**: `oper_32b.h` の 3 引数マクロと `basop_mpy.c` の 2 引数実装が衝突。
- **修正**: タイプシュム `evs_fx_typedef_shim.h` で `#undef Mpy_32_16` してリネーム。2 引数を必要とするソースには `TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME` を付与。

### 5.3 `basop_util.c` から float `rom_com.h` が漏れる
- **現象**: FX ビルド中に float 専用構造体/テーブルが参照できずコンパイルエラー。
- **修正**: `TELEPHONY_EVS_FX_BASOP_UTIL_SHIM` 定義で不要な float ヘッダブロックをスキップ。

### 5.4 `lerp_fx` の負インデックス・ステージングバグ
- **現象**: 最初の実装で境界付近の補間がずれていた。
- **修正**: サンプリング位置の計算とループ範囲を調整。

### 5.5 `Dot_productSq16HQ` の指数セマンティクス
- **現象**: レビューで `exp` が raw-normalized 規約から 31 bit ずれていると指摘。
- **修正**: `basop_extra_norm_ll_q31()` の `*exp = -shift` を `*exp = 31 - shift` に変更。

### 5.6 `BASOP_Util_Divide3232_Scale` 内の `div_s` 丸め
- **現象**: ローカル `div_s` が `+ den/2` で丸めていたため upstream と不一致。
- **修正**: 切り捨て除算に変更し、exact equality のみ `MAX_16` で飽和。

### 5.7 符号付き左シフトの UB
- **現象**: `basop_extra_l_shl()` が負数を左シフトしていた。
- **修正**: unsigned 32-bit パターンでシフトし、飽和後に符号解釈するよう変更。

### 5.8 `noisy_speech_detection` の重複定義
- **現象**: `fdcng_enc_fx.c` と `fdcng_dec_fx.c` の両方に `noisy_speech_detection` を実装したため `LNK4006` 警告。
- **修正**: エンコーダ側の定義を削除し、デコーダ側に集約。

## 6. 動作確認

### 6.1 リンク確認

```text
[260/261] Linking CXX static library TelephonyDSP.lib
[261/261] Linking CXX executable TelephonyRunner.exe
```

`LNK1120` は **0** となり、`TelephonyRunner.exe` が生成された。残存するリンク警告は `Log2_norm_lc` / `Pow2` の重複定義（`basop_util.c` と `basic_math/log2.c` / `basic_math/math_op.c`）のみで、動作に影響しない既知のもの。

### 6.2 実行確認

```powershell
./out/build/x64-release/TelephonyRunner.exe --help
./out/build/x64-release/TelephonyRunner.exe tmp/test_16k.wav --mode evs_native --evs-sr 16000 --evs-br 13200 --evs-bw WB
```

- `--help` は正常に表示される。
- `evs_native` モードはクラッシュせずに終了し、出力 WAV を生成する。
- ただし、現時点では **出力 WAV が 0 バイト** になる。これは `enc_acelp_tcx_main` などのコア符号化パスがリンク unblock stub のためであり、今後の数値的実装で音声が出力されるようになる。

## 7. 未解決の機能的課題（リンクは通ったが stub あり）

| 領域 | 残課題 | 影響 |
|---|---|---|
| ACELP/TCX コア符号化 | `E_ACELP_1`, `E_GAIN_closed_loop_search`, `encode_acelp_gains`, `enc_acelp_tcx_main` などが stub | `evs_native` 出力が 0 バイト、音声品質未実装 |
| FD-CNG | `perform_noise_estimation_enc`, `FdCng_encodeSID`, `generate_comfort_noise_*`, `generate_masking_noise` などが stub | 快適雑音生成/SID 符号化が機能しない |
| デコーダ LPD 状態機 | `open_decoder_LPD` 以外の多くが stub | フレーム喪失時のコンシールメントなどが未実装 |
| VAD/前処理 | `vad_proc`, `analy_sp`, `MDCT_selector` などが stub | DTX/VAD 判定が固定値に近い動作 |

これらは **リンクエラー解決後の機能実装フェーズ** として別途対応する。

## 8. 変更中ファイル一覧

### コミット済み（`feature/evs-fx-link-helpers`）
- `acelp_core_fx.c`
- `basic_utils_fx.c`
- `basop1616_fx.c`
- `basop_extra_fx.c`
- `cldfb_fx.c`
- `core_enc_vad_fx.c`
- `dec_postfilter_fx.c`
- `fdcng_dec_fx.c`
- `fdcng_enc_fx.c`
- `get_gain_fx.c`
- `lerp_fx.c`
- `pitch_fx.c`
- `tec_tfa_tbe_fx.c`
- `tns_base_fx.c`
- `EVS_FX_PROGRESS.md`
- `.gitignore`
- `cmake/3gpp-evs.cmake`

### 未変更（読み取り専守）
- `external/3gpp-evs/**` — 一切編集していない。

## 9. 備考

- float EVS ビルドは既に動作しており、FX ビルドは実験的/WIP。
- 各バッチはリンクを通すことが第一目標。固定小数点の bit-exact 再現は第二目標として、レビュー時に数値的問題を修正している。
- ログは `tmp/` に蓄積。`.gitignore` で無視済み。
