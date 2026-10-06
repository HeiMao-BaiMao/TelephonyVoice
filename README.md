# TelephonyVoice

固定電話・携帯電話の音声経路をエミュレートする VST3 プラグイン(+ CLI ランナー)です。
UI はコーデック規格名ではなく「`in -> 交換局 -> out`」「固定電話」「4G携帯」「5G携帯」
といった経路ラベルで操作します。個人利用ビルドは同梱の 3GPP 参照実装を使用でき、
配布ビルドではそれらのモードを隠して配布可能な代替実装のみを使います。

- 作業履歴の詳細: [docs/CHANGELOG.md](docs/CHANGELOG.md)
- 実装ロードマップ (Tier 0〜4): [docs/ROADMAP.md](docs/ROADMAP.md)

## クイックスタート

```pwsh
# 1. サブモジュールごとクローン
git clone --recurse-submodules <this-repo> TelephonyVoice
cd TelephonyVoice

# 2. 構成 (VS 18 / Ninja)
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" && cmake --preset x64-release"

# 3. ビルド
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" && cmake --build out/build/x64-release --parallel"
```

成果物:

* `out/build/x64-release/TelephonyRunner.exe` – CLI
* `out/build/x64-release/VST3/TelephonyVoice.vst3` – プラグイン (読み込み可能な
  モジュール本体。`VST3/Release/TelephonyVoice.vst3/` はアイコン等のリソース
  置き場で、その中にバイナリはありません)

CLI は 16-bit PCM WAV を入力すると、各モードの出力 `<入力名>.<モード>.wav` を
まとめて生成します:

```pwsh
.\out\build\x64-release\TelephonyRunner.exe path\to\input.wav
```

コーデック設定は CLI フラグでも変更できます
(`--g711-law` / `--amr-nb-mode` / `--amr-wb-mode` / `--opus-bitrate` /
`--opus-bw` / `--evs-dtx-sid-interval` / `--evs-sc-vbr` など。
`TelephonyRunner.exe --help` を参照)。

## ルートとモード

VST の経路は独立に劣化する 2 本のレグとしてモデル化されています:

```text
in -> 交換局 -> out
```

UI パラメータ:

* `in` / `out` – 固定電話 / 2G携帯 / 3G携帯 / 4G携帯 / 5G携帯 (+ 個人ビルドでは 5G携帯(精密))
* `劣化区間: in -> 交換局 -> out` – 両区間 / in→交換局のみ / 交換局→out のみ / なし
* `パケットロス` – 選択した劣化区間にパケット/フレーム消失として適用
* `通信劣化` – 経路帯域の狭窄とバースト損失の増加
* コーデック詳細 – G.711 Law / AMR-NB・AMR-WB モード / Opus ビットレート・帯域 /
  EVS サンプルレート・ビットレート・帯域・DTX SID 間隔・SC-VBR

ユーザー向けエンドポイントは内部的に次のコーデック世代モードへマップされます:

| `EraMode`                | サンプルレート | コーデック             | 実装                                   |
| ------------------------ | ----------- | ---------------------- | ------------------------------------- |
| `PSTN_G711`              | 8 kHz       | G.711 µ-law / A-law    | ✅ (パブリックドメイン, Sun)           |
| `GSM_FR`                 | 8 kHz       | GSM 06.10              | ✅ (libgsm, 寛容ライセンス)            |
| `AMR_NB_3G`              | 8 kHz       | AMR-NB MR475~MR122 (選択可) | ✅ (opencore-amr, Apache 2.0)      |
| `AMR_WB_VOLTE`           | 16 kHz      | AMR-WB 6.60~23.85 kbps (選択可) | ✅ (vo-amrwbenc + opencore-amrwb) |
| `EVS_LIKE`               | 32 kHz      | *フィルタのみの代替*    | ❌ (配布可能)                          |
| `EVS_NATIVE`             | 8/16/32/48  | 3GPP EVS 参照実装       | ✅ (3GPP TS 26.443 v12.7.0/v13.3.0)    |
| `Bypass`                 | ホスト      | –                      | –                                     |
| `OPUS_VOIP` *(実験的)*   | 48 kHz      | Opus (VOIP アプリケーション, 6~256 kbps 選択可) | ✅ (opus, BSD) — 非配布ビルドのみ |
| `EVS_JBM` *(実験的)*     | 8/16/32/48  | EVS + Stage-1 JBM/VoIP アダプタ | ✅ (3GPP EVS + `EvsRXlib`) — `Mobile5GJbm` エンドポイント経由で `5G携帯 (JBM)`、`TELEPHONY_USE_EVS_JBM=ON` のみ |

バイパスはレイテンシ補正付きです(プラグインがホストへ報告するレイテンシと同じ
遅延をドライ信号にも与えるので、バイパス切替で音の位置がずれません)。

## ビルドオプション

| オプション                  | 既定値  | 効果                                                                 |
| -------------------------- | ------- | ---------------------------------------------------------------------- |
| `TELEPHONY_USE_EVS_FX`     | OFF     | 実験的な固定小数点 EVS (TS 26.442)。FX ライブラリ・`TelephonyDSP`・`TelephonyRunner` まで**ビルドとリンクは通る**(`LNK1120` = 0)が、固定小数点コアの主要部が未移植のスタブのままで、実音声を通すと動作中にクラッシュする。詳細は [docs/CHANGELOG.md](docs/CHANGELOG.md) と [EVS_FX_PROGRESS.md](EVS_FX_PROGRESS.md)。 |
| `TELEPHONY_USE_EVS_JBM`    | OFF     | 3GPP `EvsRXlib` を包む実験的な EVS Stage-1 JBM/VoIP 受信アダプタ (`evs_api_rx`) と `EVSJbmSmoke` をビルド。`TELEPHONY_DISTRIBUTION_BUILD`・`TELEPHONY_USE_EVS_FX` とは併用不可。 |
| `TELEPHONY_DISTRIBUTION_BUILD` | OFF | AMR/AMR-WB/EVS 参照実装を除外し、配布可能なモードのみを公開。 |
| `TELEPHONY_EXPERIMENTAL_NETWORK` | ON (配布ビルドでは強制 OFF) | SpeexDSP + Opus をビルドし `OPUS_VOIP` モードを公開 (BSD ライセンス)。 |

CMake プリセット:

| プリセット           | 内容                                       |
| ------------------- | ------------------------------------------ |
| `x64-release`       | 通常(個人利用)ビルド                      |
| `x64-release-dist`  | 配布ビルド (`TELEPHONY_DISTRIBUTION_BUILD=ON`) |
| `x64-release-jbm`   | EVS JBM 実験ビルド (`TELEPHONY_USE_EVS_JBM=ON`) |
| `x64-debug` ほか    | デバッグ用                                  |

## テストと CI

スモークテスト (`tests/TelephonyDspSmoke.cpp`) は、ビルド構成で到達可能な全
`EraMode` に音声的なバースト信号を通して「有限かつ無音でない出力」を確認し、
さらに非配布ビルドでは EVS C API を WB 16 kHz / SWB 32 kHz(プラグイン既定)で
DTX 有効のままラウンドトリップします(SID / NO_DATA フレームと PLC 経路を含む)。

```pwsh
ctest --test-dir out/build/x64-release --output-on-failure
```

JBM ビルドでは `EVSJbmSmoke` も `evs_jbm_smoke` として ctest に登録されます。
`TELEPHONY_USE_EVS_FX=ON` の構成では、固定小数点 EVS がまだ実行時に安定しない
ため ctest 自体を登録していません ([CMakeLists.txt](CMakeLists.txt) の該当ブロック参照)。
GitHub Actions (`.github/workflows/ci.yml`) が push / PR ごとに
`x64-release` / `x64-release-dist` / `x64-release-jbm` の 3 構成を
ビルド+テストします。

## コード構成

```
.
├── CMakeLists.txt
├── CMakePresets.json
├── .github/workflows/ci.yml  # CI (3構成のビルド+ctest)
├── cmake/
│   ├── 3gpp-evs.cmake     # evs-lib-{com,enc,dec}[-fx] をビルド
│   ├── g711.cmake
│   ├── libgsm.cmake
│   ├── opencore-amr.cmake
│   ├── opus.cmake         # external/opus を programs/tests OFF で add_subdirectory
│   ├── r8brain.cmake
│   ├── speexdsp.cmake     # libspeexdsp の必要ソースのみの静的ライブラリ
│   └── vo-amrwbenc.cmake
├── evs_api.h              # 3GPP EVS ラッパーの公開 C API
├── evs_api.c              # 浮動小数点版(完全インメモリ、ファイルI/Oなし)
├── evs_api_fx.c           # 固定小数点版(実験的 / リンクは可、実行時は未完成)
├── *_fx.c (ルート直下)     # 固定小数点コアの親リポジトリ側ヘルパー(大半がスタブ)
├── helpers/               # evs-float-acelp: float 版 ACELP 探索を FX から利用する糊
├── EVS_FX_PROGRESS.md     # 固定小数点 EVS の作業メモ(リンク到達点と残スタブ)
├── evs_api_rx.h / .c      # EVS JBM/VoIP 受信アダプタ (TELEPHONY_USE_EVS_JBM)
├── dsp/                   # 経路対応 SignalProcessor、コーデック別クラス、
│                          # PLC、リサンプラ/フィルタチェーン(クラス毎に27ファイル)
├── TelephonyVoice.h       # VST3 プロセッサ + エディットコントローラ
├── TelephonyVoice.cpp     # VST3 グルー
├── TelephonyRunner.cpp    # CLI: 入力 WAV に全モードを適用
├── tests/
│   └── TelephonyDspSmoke.cpp  # ctest スモークテスト
├── docs/
│   ├── CHANGELOG.md       # 作業履歴の詳細ログ
│   └── ROADMAP.md         # 実装ロードマップ (Tier 0〜4)
└── external/
    ├── 3gpp-evs/          # 3GPP EVS 参照実装 (TS 26.443 + 26.442)
    ├── G711_G72x/         # G.711/G.721/G.723
    ├── libgsm/            # GSM 06.10
    ├── opencore-amr/      # AMR-NB + AMR-WB デコーダ
    ├── opus/              # Opus コーデック (BSD)
    ├── r8brain/           # サンプルレートコンバータ
    ├── speexdsp/          # SpeexDSP (BSD、preprocess/jitter/FFT のみビルド)
    ├── vo-amrwbenc/       # AMR-WB エンコーダ
    └── vst3sdk/           # Steinberg VST3 SDK
```

## 配布ビルド(非 EVS 版)

特許・3GPP メンバー関連の制約があるコーデック (AMR/AMR-WB/EVS) を完全に除外し、
固定電話・2G・5G 代替ルートのみを載せた配布用ビルドです。

* `TELEPHONY_DISTRIBUTION_BUILD=ON` で `cmake/vo-amrwbenc.cmake`、
  `cmake/opencore-amr.cmake`、`cmake/3gpp-evs.cmake` を除外。
* `TelephonyDSP` は AMR/AMR-WB/EVS のシンボルなしでコンパイルされ、AMR 代替は
  既存のリサンプル/フィルタ経路でフレームをパススルーします。
* `EraMode::EVS_NATIVE` は配布ビルドでは `EVS_LIKE` にエイリアスされます。
* VST3 UI に表示されるのは固定電話 / 2G携帯 / 5G携帯(代替)のみ。
  3G・4G・5G精密は配布 UI から到達できません。
* `TelephonyRunner` も G.711 / GSM / EVS-Like のみを列挙します。

## ライセンスと特許の注意

個人利用の範囲では、すべてのライブラリは寛容なライセンス
(Apache 2.0 / MIT / パブリックドメイン / 独自の寛容ライセンス)です。ただし
**AMR / AMR-WB / EVS のコーデック特許**(VoiceAge、Fraunhofer、NTTドコモ、
Ericsson、Nokia など)は商用配布に適用されます。配布には
`TELEPHONY_DISTRIBUTION_BUILD=ON` を使い、これらのコーデック実装を含まない
ビルドにしてください。

通常(個人)ビルドはテスト用に `5G携帯(精密)` / EVS Native を VST3 UI に
公開しますが、**このビルドは配布してはいけません**。配布ビルドでは
`EVS_NATIVE` が `EVS_LIKE` にエイリアスされ、EVS 参照実装はリンクも呼び出しも
されません。

## 実験的機能の概要

* **SpeexDSP + Opus** (`TELEPHONY_EXPERIMENTAL_NETWORK`, 既定 ON):
  BSD ライセンスの 2 ライブラリを非配布ビルドに追加。`OPUS_VOIP` モード
  (48 kHz / 20 ms / VOIP アプリケーション、in-band FEC + DTX + 決定論的
  ジッタバッファ付きのパケット化トランスポートをシミュレート)と、
  `OPUS_VOIP` / `EVS_LIKE` の DTX を駆動するエナジー VAD
  (`SpeexDSPAux`) を提供します。`OPUS_VOIP` は現状 CLI からのみ到達可能で、
  VST の経路リストには追加していません。
* **EVS JBM** (`TELEPHONY_USE_EVS_JBM`, 既定 OFF): 3GPP `EvsRXlib` を使う
  受信側ジッタバッファ(JBM)アダプタ。`5G携帯 (JBM)` エンドポイントと
  `EVSJbmSmoke` テストを追加します。浮動小数点 EVS 専用・非配布のみ。
* **固定小数点 EVS** (`TELEPHONY_USE_EVS_FX`, 既定 OFF): TS 26.442 v16.4.0 の
  移植作業中。ビルドとリンクは通ります。**短い入力 (〜25 フレーム程度) は正しく
  動作し、float 参照実装と相関 0.994・レベルも同等**です。ただし約 32 フレームを
  超えると内部のメモリ破壊で異常終了し (入力長だけで決まる再現性のある不具合)、
  DTX も FD-CNG スタブを避けるため強制 OFF です。残作業は FD-CNG / LPD 状態機械 /
  TCX 系スタブの移植と、破壊箇所の特定 (デバッガまたは検査ビルド)。
  実測値と経緯は [EVS_FX_PROGRESS.md](EVS_FX_PROGRESS.md) §0。

詳細な経緯・設計メモはすべて [docs/CHANGELOG.md](docs/CHANGELOG.md) にあります。
