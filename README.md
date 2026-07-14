# ExpressLRS Realtime Recorder

USBゲームパッドをExpressLRS送信機として使用するアダプター。
Raspberry Pi PicoでゲームパッドのUSB HID入力を取得し、CRSFプロトコルに変換してExpressLRS送信モジュールに送信する。

## システム構成

```
USBゲームパッド
    ↓ USB Host (HID, micro-USB)
Raspberry Pi Pico
    ├→ GP0 UART0 TX (CRSF 921.6kbps 反転 半二重) → BetaFPV Nano TX Module V2 → ExpressLRS → BetaFPV Pavo Pico
    └→ GP4/5 UART1 (921600bps) → USB-シリアル変換 → PC (デバッグ/値確認・ログ記録)
```

操作を Pico 内蔵フラッシュに記録し、再生できる（GP2ボタンでモード切替）。詳細は「動作モード」参照。

## ハードウェア

| コンポーネント | 説明 |
|---------------|------|
| Raspberry Pi Pico | メインマイコン |
| USBゲームパッド | 入力デバイス（HID互換） |
| BetaFPV Nano TX Module V2 | ExpressLRS送信モジュール |
| BetaFPV Pavo Pico | 機体 |

## ピン配置

| Pico Pin | 接続先 | 説明 |
|----------|--------|------|
| USB (micro-USB) | ゲームパッド | USB Host |
| GP0 (UART0 TX) | Nano TX | CRSF出力 (921.6kbps 反転 半二重) |
| GP1 (UART0 RX) | Nano TX | 半二重テレメトリ受信 |
| GP4 (UART1 TX) | USB-シリアル変換 → PC | デバッグ出力 (921600bps) |
| GP5 (UART1 RX) | USB-シリアル変換 → PC | デバッグ入力（コマンド） |
| GP2 | タクトSW → GND | モード切替ボタン（内部プルアップ） |
| GP10 / GP11 / GP12 | LED(330Ω) → GND | モードLED 赤=記録 / 緑=再生 / 青=単純 |
| 内蔵LED | - | 状態表示（接続:点灯 / 未接続:点滅 / 再生:速点滅） |

## ビルド

### 必要環境

- [Pico SDK](https://github.com/raspberrypi/pico-sdk)
- CMake 3.13以上
- ARM GCC Toolchain

### Picoファームウェアのビルド

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### テストのビルド・実行

```bash
mkdir build_test && cd build_test
cmake -DBUILD_TESTS=ON ..
make -j$(nproc)
ctest --output-on-failure
```

## 書き込み

1. PicoのBOOTSELボタンを押しながらUSB接続
2. `RPI-RP2`としてマウントされることを確認
3. 書き込みスクリプトを実行:

```bash
./scripts/flash.sh
```

または手動で:

```bash
cp build/expresslrs_realtime_recorder.uf2 /Volumes/RPI-RP2/
```

## シリアルモニター

```bash
./scripts/monitor.sh
```

または:

```bash
screen /dev/cu.usbserial-* 921600
```

終了: `Ctrl+A`, `K`

コマンド: `d`=スナップショット / `s`=ストリーム / `p`=再生start/stop(PLAYBACK) / `?`=ヘルプ

## 動作モード（単純 / 記録 / 再生）

GP2ボタンで3モードを循環し、GP10-12のLEDで表示する。起動時は安全な**単純(SIMPLE)**。

| モード | LED | 動作 |
|--------|-----|------|
| SIMPLE (単純) | GP12 青 | ゲームパッド → CRSF パススルー |
| RECORD (記録) | GP10 赤 | パススルー＋操作を記録（50Hz, Pico内蔵フラッシュへ） |
| PLAYBACK (再生) | GP11 緑 | フラッシュのログを CRSF として再送出 |

- **短押し**: モード循環 / **長押し(0.6秒)**: PLAYBACK中に再生 start/stop
- 記録はRECORDを抜けた時にフラッシュへ一括書出し（飛行中は書かずタイミングを保護）。
- 切断時はスロットル最小＋Arm解除のフェイルセーフを送出。

## 配線・プロトタイプ

- [`docs/perfboard-layout.html`](docs/perfboard-layout.html) — ユニバーサル基板の実寸グリッド配置図＋結線ネットリスト（ブラウザで開く）
- [`docs/hardware-validation.md`](docs/hardware-validation.md) — 実機ブリングアップ手順（CRSF反転の切り分け含む）

## プロジェクト構造

```
expresslrs_realtime_recorder/
├── CMakeLists.txt
├── README.md
├── CLAUDE.md                    # プロジェクト仕様書
├── include/
│   ├── crsf.h                   # CRSFプロトコル定義
│   ├── usb_gamepad.h            # ゲームパッドAPI
│   ├── recorder.h              # 記録/再生API
│   └── tusb_config.h            # TinyUSB設定
├── src/
│   ├── main.c                   # メインアプリ（モード/ボタン/LED含む）
│   ├── crsf.c                   # CRSF実装
│   ├── usb_gamepad.c            # USB Host実装
│   └── recorder.c              # 記録/再生（RAM蓄積→フラッシュ）
├── tests/
│   ├── test_crsf.cpp            # CRSFテスト
│   ├── test_usb_gamepad.cpp     # ゲームパッドテスト
│   └── test_recorder.cpp        # 記録ロジック（間引き/ラッチ）テスト
├── scripts/
│   ├── flash.sh                 # 書き込みスクリプト
│   └── monitor.sh               # モニタースクリプト
└── .claude/
    └── skills/
        ├── build.md             # /build スキル
        └── test.md              # /test スキル
```

## CRSFプロトコル

| 項目 | 値 |
|------|-----|
| ボーレート | 921,600 bps（Nano TX V2 モジュール用。レシーバ直結は420,000） |
| 信号 | 反転UART（Nano TX V2 の S.Port） |
| 通信方式 | 半二重（送信後テレメトリをドレイン） |
| フレームタイプ | 0x16 (RC Channels Packed) |
| チャンネル数 | 16 |
| チャンネル解像度 | 11bit (0-2047) |
| チャンネル値範囲 | 172 (min) - 992 (mid) - 1811 (max) |
| CRC | CRC8-DVB-S2 (多項式 0xD5) |

### パケット構造

```
[SYNC: 0xC8] [LEN: 24] [TYPE: 0x16] [PAYLOAD: 22bytes] [CRC: 1byte]
```

## 対応コントローラー

- Sony DualShock 4（専用パーサー）
- 汎用HIDゲームパッド

## チャンネルマッピング (Mode 2)

| CRSFチャンネル | ゲームパッド入力 | 備考 |
|---------------|-----------------|------|
| CH1 (Roll) | 右スティック X | |
| CH2 (Pitch) | 右スティック Y | 反転 |
| CH3 (Throttle) | 左スティック Y | 反転 |
| CH4 (Yaw) | 左スティック X | |
| CH5 | L2トリガー | |
| CH6 | R2トリガー | |
| CH7 (Arm) | Aボタン (Cross) | ON/OFF |
| CH8 | Bボタン (Circle) | ON/OFF |
| CH9 | Xボタン (Square) | ON/OFF |
| CH10 | Yボタン (Triangle) | ON/OFF |
| CH11 | LBボタン (L1) | ON/OFF |
| CH12 | RBボタン (R1) | ON/OFF |
| CH13-16 | 未使用 | 中央値固定 |

## 参考資料

- [CRSF Protocol Specification](https://github.com/crsf-wg/crsf/wiki)
- [ExpressLRS CRSF Implementation](https://github.com/ExpressLRS/ExpressLRS/blob/master/src/lib/CrsfProtocol/crsf_protocol.h)
- [TinyUSB Host HID Example](https://github.com/hathach/tinyusb/tree/master/examples/host/hid_controller)
- [Pico SDK Documentation](https://raspberrypi.github.io/pico-sdk-doxygen/)

## ライセンス

MIT License
