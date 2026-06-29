# ExpressLRS Realtime Recorder

## プロジェクト概要
USBゲームパッドをExpressLRS送信機として使用するアダプター。
Raspberry Pi PicoでゲームパッドのUSB HID入力を取得し、CRSFプロトコルに変換してExpressLRS送信モジュールに送信する。

## システム構成

```
USBゲームパッド
    ↓ USB Host (HID)
Raspberry Pi Pico
    ├→ UART TX (CRSF) → BetaFPV Nano TX Module V2 → ExpressLRS → BetaFPV Pavo Pico
    └→ USB CDC → PC (フライトログ記録)
```

## ハードウェア
- **マイコン**: Raspberry Pi Pico
- **入力**: USBゲームパッド（USB HID）
- **送信モジュール**: BetaFPV Nano TX Module V2
- **機体**: BetaFPV Pavo Pico

## 開発環境
- **SDK**: Pico SDK (C/C++)
- **ビルド**: CMake + Make

## ピン配置
| Picoピン | 機能 | 接続先 |
|----------|------|--------|
| USB | USB Host | ゲームパッド |
| GP0 | UART0 TX | Nano TX (CRSF 921.6kbps, 反転) |
| GP1 | UART0 RX | (未使用) |
| GP4 | UART1 TX | PC (デバッグ 115200bps) |
| GP5 | UART1 RX | PC |
| LED | 状態表示 | 接続時:点灯 / 未接続:点滅 |

## CRSFプロトコル
- ボーレート: 921600 bps（ELRS V3.x の Nano TX V2 モジュール用。レシーバ直結は420000）
- 信号: Nano TX V2 の S.Port は反転UART（Pico側で反転）
- 半二重: RCフレーム送信後にテレメトリ応答を読み捨ててバス衝突を防止
- パケット形式: [SYNC][LEN][TYPE][PAYLOAD][CRC]
- 主要フレームタイプ:
  - 0x16: RC Channels Packed (16ch, 11bit each)

## Skills
- `/build` - ファームウェアをビルド
- `/test` - ユニットテストを実行

## 手動操作スクリプト

### Flash書き込み
```bash
./scripts/flash.sh
```
- Picoを**BOOTSELボタンを押しながら**USB接続
- RPI-RP2としてマウントされたことを確認
- スクリプトを実行

### シリアルモニター
```bash
./scripts/monitor.sh [ボーレート]
```
- デフォルト: 115200 bps
- 終了: `Ctrl+A`, `K` (screenの場合)
