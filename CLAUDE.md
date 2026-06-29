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
- デフォルト: 921600 bps（デバッグUART GP4/5）
- 終了: `Ctrl+A`, `K` (screenの場合)
- コマンド入力: `d`=スナップショット, `s`=連続ストリームON/OFF, `?`=ヘルプ

### リアルタイム値ビューア（方式C）
```bash
python3 scripts/live_view.py [--port /dev/tty.usbmodemXXXX] [--baud 921600]
```
- 各CRSFチャンネルをバーゲージでライブ表示（要 `pip install pyserial`）
- キー: `s`=ストリーム, `d`=スナップショット, `q`=終了

## デバッグ/値確認の仕組み
処理パイプラインの各ポイントで値を検証できる:
①生HIDレポート → ②デコード後state → ③CRSFチャンネル → ④パック後payload →
⑤フレーム全体+CRC検証 → ⑥ドレインしたテレメトリ

- **スナップショット(A)**: `d` で次の1フレームの①〜⑥を全段HEXダンプ（CRC一致もOK/NG表示）
- **連続ストリーム(C)**: `s` でON/OFF。25Hzで `D,...` CSVを出力し `live_view.py` が描画。デフォルトOFF（実飛行時はオーバーヘッド0）
- デバッグUARTは500Hz送信を崩さないよう921600bpsで運用（1行≈1.4ms < 2ms）
