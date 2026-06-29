# ExpressLRS Realtime Recorder

USBゲームパッドをExpressLRS送信機として使用するアダプター。
Raspberry Pi PicoでゲームパッドのUSB HID入力を取得し、CRSFプロトコルに変換してExpressLRS送信モジュールに送信する。

## システム構成

```
USBゲームパッド
    ↓ USB Host (HID)
Raspberry Pi Pico
    ├→ UART TX (CRSF 921.6kbps 反転) → BetaFPV Nano TX Module V2 → ExpressLRS → BetaFPV Pavo Pico
    └→ UART (115200bps) → PC (フライトログ記録)
```

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
| USB | ゲームパッド | USB Host |
| GP0 (UART0 TX) | Nano TX | CRSF出力 (921.6kbps 反転) |
| GP1 (UART0 RX) | - | 未使用 |
| GP4 (UART1 TX) | PC | デバッグ出力 (115200bps) |
| GP5 (UART1 RX) | PC | デバッグ入力 |
| LED | - | 状態表示（接続:点灯/未接続:点滅） |

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
screen /dev/tty.usbmodem* 115200
```

終了: `Ctrl+A`, `K`

## プロジェクト構造

```
expresslrs_realtime_recorder/
├── CMakeLists.txt
├── README.md
├── CLAUDE.md                    # プロジェクト仕様書
├── include/
│   ├── crsf.h                   # CRSFプロトコル定義
│   ├── usb_gamepad.h            # ゲームパッドAPI
│   └── tusb_config.h            # TinyUSB設定
├── src/
│   ├── main.c                   # メインアプリケーション
│   ├── crsf.c                   # CRSF実装
│   └── usb_gamepad.c            # USB Host実装
├── tests/
│   ├── test_crsf.cpp            # CRSFテスト
│   └── test_usb_gamepad.cpp     # ゲームパッドテスト
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
