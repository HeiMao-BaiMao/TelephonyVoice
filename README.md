# TelephonyVoice

固定電話・携帯電話の音声経路をエミュレートする VST3 プラグイン(+ CLI ランナー)です。
UI はコーデック規格名ではなく「`in -> 交換局 -> out`」「固定電話」「4G携帯」「5G携帯」
といった経路ラベルで操作します。個人利用ビルドは同梱の 3GPP 参照実装を使用でき、
配布ビルドではそれらのモードを隠して配布可能な代替実装のみを使います。

- 作業履歴の詳細: [docs/CHANGELOG.md](docs/CHANGELOG.md)
- 実装ロードマップ (Tier 0〜4): [docs/ROADMAP.md](docs/ROADMAP.md)

## 専用エディタとサポート範囲

VST3 ホストでプラグインを開くと、`in → 交換局 → out` の経路、通信劣化、
ミックス／出力、コーデック詳細をまとめた専用エディタを表示します。
ホストのオートメーションおよび保存状態と連動し、現在の経路で使わない
コーデック設定は無効表示になります。配布ビルドでは利用できない
コーデック設定を表示しません。Opus は引き続き CLI 専用です。

注意: 通常ビルド (JBM 無効) の経路リストと音声処理の値変換のずれを修正しました。
パラメータ ID と保存済みの経路整数は維持しますが、旧版で記録した経路の
オートメーションは、以前の誤った音声経路と異なる結果になる場合があります。
既存プロジェクトでは in / out の選択とオートメーションを確認してください。

これは DAW 内の音声エフェクトです。電話発信、録音、外部ネットワーク通信は
行いません。ロードマップの Tier 1〜4 は将来の研究・拡張項目を含み、
すべてを実装済みとするものではありません。固定小数点 EVS は引き続き実験中です。

## クイックスタート

```pwsh
# 必要: CMake 3.25 以上、C++20 コンパイラ、Ninja
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
* `out/build/x64-release/VST3/Release/TelephonyVoice.vst3` – プラグイン

CLI は 16-bit 整数 PCM WAV (8〜192 kHz、1〜32 ch) を入力すると、各モードの出力 `<入力名>.<モード>.wav` を
まとめて入力ファイルと同じフォルダへ生成します。壊れた WAV、無効なオプション、
読み書き失敗は非ゼロ終了コードになります。出力は変換成功後に置き換えるため、
処理が失敗しても既存の出力ファイルを壊しません。非空の入力には末尾のコーデック音声を
排出する 1〜10 秒のテールが追加されます (入力が空なら空の WAV のままです):

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

EVS は選択したサンプルレートに合わせて帯域を制限し、5.9 kbps は NB/WB の
SC-VBR として処理します。8 kHz または NB の上限は 24.4 kbps です。CLI は
無効な組合せを出力作成前に拒否します。VST3 の要求値が NB 上限を超えた場合は、
音声処理で 24.4 kbps に決定的に制限し、エディタに注意を表示します。

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
音声経路のバッファは 150 ms とし、ホストへ同じ遅延を報告します。
16-bit 音声向けのリサンプラ設定 (96 dB 阻止帯域、10% 遷移帯域、最小位相) を使い、
経路切替時はドライ／ウェットを一緒に初期化します。CLI で追加の遅延シミュレーションを
無効化しても、コーデック経路の処理に必要なバッファは維持します。


## ビルドオプション

| オプション                  | 既定値  | 効果                                                                 |
| -------------------------- | ------- | ---------------------------------------------------------------------- |
| `TELEPHONY_BUILD_PLUGIN` | ON | VST3 と GUI をビルド。OFF は SDK を利用する headless 回帰テストと CLI のみ。 |
| `TELEPHONY_VALIDATE_PLUGIN` | ON | VST3 ビルド後に Steinberg SDK validator でプラグインを検査。 |
| `TELEPHONY_USE_EVS_FX`     | OFF     | 実験的な固定小数点 EVS (TS 26.442)。FX ライブラリと `TelephonyDSP` まではビルドできるが、`TelephonyRunner` の最終リンクが未解決シンボルでブロック中(詳細は [docs/CHANGELOG.md](docs/CHANGELOG.md))。 |
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
GitHub Actions (`.github/workflows/ci.yml`) が push / PR ごとに
`x64-release` / `x64-release-dist` / `x64-release-jbm` の Windows 3 構成を
ビルド+テストします。

### GUI なしのローカル回帰テスト

Linux などで VSTGUI の開発ライブラリを入れずに音声処理／CLI／VST 状態管理を
検証する場合は、プラグインのビルドだけを無効化できます。VST3 SDK を含む
サブモジュールは必要です。この構成は GUI の表示確認を代替しません。

```sh
cmake -S . -B out/build/headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DTELEPHONY_BUILD_PLUGIN=OFF
cmake --build out/build/headless --parallel
ctest --test-dir out/build/headless --output-on-failure
```

`dsp_regressions`、`plugin_regressions`、`runner_regressions`、非配布ビルドの
`evs_configurations` は合成信号と一時 WAV を
使います。マイク、実通話、外部サービスへの接続は不要です。Windows の VST3
ビルド 3 構成と Linux の GUI なし 3 構成を CI で検証します。

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
├── evs_api_fx.c           # 固定小数点版(実験的 / リンクブロック中)
├── evs_api_rx.h / .c      # EVS JBM/VoIP 受信アダプタ (TELEPHONY_USE_EVS_JBM)
├── dsp/                   # 経路対応 SignalProcessor、コーデック別クラス、
│                          # PLC、リサンプラ/フィルタチェーン(クラス毎に27ファイル)
├── TelephonyVoice.h       # VST3 プロセッサ + エディットコントローラ
├── TelephonyVoice.cpp     # VST3 グルー
├── TelephonyEditor.cpp/.h # VSTGUI エディタ
├── resources/telephonyvoice.uidesc # エディタのレイアウト
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
  移植作業中。コンパイルは通るものの最終リンクが未解決シンボルでブロックされて
  おり、出荷可能な状態ではありません。浮動小数点版が引き続きサポート対象です。

詳細な経緯・設計メモはすべて [docs/CHANGELOG.md](docs/CHANGELOG.md) にあります。
