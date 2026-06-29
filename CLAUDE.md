# ExpressLRS Realtime Recorder

## プロジェクト概要
USBゲームパッドをExpressLRS送信機として使用するアダプター。
Raspberry Pi PicoでゲームパッドのUSB HID入力を取得し、CRSFプロトコルに変換してExpressLRS送信モジュールに送信する。

## システム構成

```
USBゲームパッド
    ↓ USB Host (HID, micro-USBポート)
Raspberry Pi Pico
    ├→ GP0 UART0 TX (CRSF 921.6kbps 反転 半二重) → BetaFPV Nano TX Module V2 → ExpressLRS → BetaFPV Pavo Pico
    └→ GP4/5 UART1 (921600bps) → USB-シリアル変換 → PC (デバッグ/値確認・ログ記録)
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
| USB (micro-USB) | USB Host | ゲームパッド（OTG変換アダプタ経由） |
| GP0 (pin1) | UART0 TX | Nano TX S.Port/CRSF信号 (921.6kbps, 反転, 半二重) |
| GP1 (pin2) | UART0 RX | (半二重テレメトリ受信) |
| GP4 (pin6) | UART1 TX | PC デバッグ/ログ (921600bps, USB-シリアル変換経由) |
| GP5 (pin7) | UART1 RX | PC (コマンド入力) |
| VBUS (pin40) | 電源入力 | 降圧モジュール出力5V（LiPoから降圧、Pico＋ゲームパッド給電） |
| GND (pin3 等) | グランド | 共通GND（下記参照） |
| LED | 状態表示 | 接続時:点灯 / 未接続:点滅 |

## 電源・配線

### 電源系統（単一LiPo + 降圧モジュール）
LiPo 1個から分岐して全体を給電する。

- **Nano TX Module V2**: LiPo(**7〜13V / 2S〜3S**)を **VBAT に直結**。5Vでは動作しないためここはLiPo直。
- **Pico＋ゲームパッド**: LiPoを **降圧(buck)モジュールで5.0Vに落とし**、**VBUS(pin40)** へ供給。VBUS→USBポート経由でゲームパッドへ給電、同時に内蔵D1→VSYS→SMPSでPico本体も駆動。
  - VSYS(pin39)給電はNG（D1が逆流を防ぎUSBポートに5Vが回らずゲームパッドへ給電できない）。
- **＋側（buck出力5V と LiPo+ 7-13V）は絶対に接続しない**。特にLiPo+をPico VBUS/VSYSに繋ぐのは厳禁（最大5.5V、即破壊）。

#### 降圧モジュール
- 入力範囲: LiPo満充電をカバー（3Sなら〜15V入力対応）。出力: **5.0V**。出力電流: **1A以上**（Pico ~50mA＋ゲームパッド最大~500mA＋余裕、2-3A推奨）。MP1584 / LM2596 等で可。
- ⚠️ **最重要**: 可変モジュールは初期出力が12V等のことがある。**Picoに繋ぐ前にテスターで出力を5.0Vに調整**してから配線する（誤って高電圧をVBUSに入れるとPico即死）。
- 非絶縁buckは入力GND/出力GNDが共通＝配線するだけで共通GNDになる（好都合）。

### 配線図
```
LiPo(+) ──┬─────────────────────────► Nano TX VBAT (7-13V 直結)
          └──► [Buck IN+]
               [Buck OUT+ = 5.0V] ───► Pico VBUS (pin40) ──(内部D1)──► VSYS → 3.3V → ゲームパッド

Pico GP0 (pin1) ───────────────────► Nano TX S.Port/CRSF信号 (半二重1本, 反転)

LiPo(−) ──┬─────────────────────────► Nano TX GND       ← TXの大電流リターンはLiPo直結
          ├──► [Buck IN−/OUT− 共通]
          └─────────────────────────► Pico GND (pin3)    ← 信号の0V基準用に1本

[ゲームパッド] ──OTG変換(micro-B→A)──► Pico micro-USBポート
```

### GND（スター結線）
- 共通GNDノードは **LiPo− を起点（スター）** にまとめる。
- **Nano TX のGNDはLiPo−に直結**（送信時の大電流がPico基板を通らないようにする）。
- buckのGND(IN−/OUT−)もLiPo−へ。そこから **Pico GND(pin3)へ1本**引いて信号の0V基準を共有する。
- pin3 はGP0(CRSF信号)の隣で戻り経路が短く好適（GNDは pin 3/8/13/18/23/28/33/38 すべて内部共通）。

### 電源運用メモ
- **3S推奨**: 2Sは放電末期に7V付近まで下がりNano TXの下限(7V)に触れることがある。3Sは放電中ほぼ9V以上で安定。buckは入力6V程度まで5Vを作れるため、律速はTXの7V下限。
- 任意: TX VBATに小容量電解コン(数百µF)で送信時ドロップ対策、LiPo+主線にヒューズ。

### 運用上の制約
- micro-USBポートはゲームパッド専有のため、**ゲームパッド接続中はPCでのフラッシュ書き込み不可**。
- 書き込み時: ゲームパッドと外部5Vを外す → BOOTSEL押しながらPCへmicro-USB接続 → `./scripts/flash.sh` → 元に戻す。
- USBが埋まるためPCログはUSB CDC不可。デバッグ/値確認は **UART1(GP4/5)** にUSB-シリアル変換を繋いで行う。

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
