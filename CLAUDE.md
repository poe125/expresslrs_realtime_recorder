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

## ピン配置（予定）
- **UART TX (CRSF)**: GPxx → Nano TX モジュール
- **USB Host**: Pico の USB ポート（ゲームパッド接続）
- **USB Device**: デバッグ/ログ用（picoprobe または USB CDC）

## CRSFプロトコル
- ボーレート: 420000 bps
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
