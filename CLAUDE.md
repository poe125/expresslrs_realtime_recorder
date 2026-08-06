# ExpressLRS Realtime Recorder

## プロジェクト概要
USBゲームパッドをExpressLRS送信機として使用するアダプター。
Raspberry Pi PicoでゲームパッドのUSB HID入力を取得し、CRSFプロトコルに変換してExpressLRS送信モジュールに送信する。

## システム構成

```
USBゲームパッド
    ↓ USB Host (HID, micro-USBポート)
Raspberry Pi Pico
    ├→ GP0 UART0 TX (CRSF 921.6kbps 反転 半二重) → BetaFPV Nano TX Module V2 → ExpressLRS → BetaFPV Pavo Pico II
    └→ GP4/5 UART1 (921600bps) → USB-シリアル変換 → PC (デバッグ/値確認・ログ記録)
```

## ハードウェア
- **マイコン**: Raspberry Pi Pico
- **入力**: USBゲームパッド（USB HID）
- **送信モジュール**: BetaFPV Nano TX Module V2
- **機体**: BetaFPV Pavo Pico II（F4 2-3S 20A AIO FC, Serial ELRS 2.4G受信機内蔵）

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
| GP2 (pin4) | モード切替ボタン | タクトSW → GND（内部プルアップ, 押下=Low） |
| GP10 (pin14) | モードLED 赤 | 記録(RECORD)点灯 (330Ω → GND) |
| GP11 (pin15) | モードLED 緑 | 再生(PLAYBACK)点灯 (330Ω → GND) |
| GP12 (pin16) | モードLED 青 | 単純(SIMPLE)点灯 (330Ω → GND) |
| VBUS (pin40) | 電源入力 | 降圧モジュール出力5V（LiPoから降圧、Pico＋ゲームパッド給電） |
| GND (pin3 等) | グランド | 共通GND（下記参照） |
| 内蔵LED | 状態表示 | 接続時:点灯 / 未接続:点滅 / 再生モード:速点滅 |

※GP2/GP10-12 の配置は参照図（`references/240529_104.jpg`, 63bit氏 R63b）に倣った空きGPIO。

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

## 動作モード（単純 / 記録 / 再生）
GP2のボタンで3モードを循環し、GP10-12のLEDで現在モードを表示する。起動時は安全な**単純(SIMPLE)**。

| モード | LED | 動作 |
|--------|-----|------|
| SIMPLE (単純) | GP12 青 | ゲームパッド → CRSF のパススルー |
| RECORD (記録) | GP10 赤 | パススラ＋操作を記録（50Hz間引き, RAM蓄積） |
| PLAYBACK (再生) | GP11 緑 | フラッシュのログを CRSF として再送出 |

### ボタン操作（GP2）
- **短押し**: モードを循環（SIMPLE→RECORD→PLAYBACK→…）
- **長押し(0.6秒)**: PLAYBACK中に再生 start/stop（PC不要でフィールド再生可）

### 記録の仕組み
- 500Hz送信を **1/10間引き=50Hz** で記録。スティックは十分な帯域、送信自体は500Hzのまま。
- スイッチ系ch(CH7-12)は間引き窓(20ms)内で**最大値ホールド（ボタンラッチ）** → 一瞬の押下も取りこぼさない。
- 記録中はRAMバッファに蓄積し、**RECORDを抜けた時にまとめてフラッシュ（上位256KB）へ書出し**。
  - 理由: RP2040はフラッシュ書込み中にXIP（コード実行）が停止し、飛行中に書くと500Hz送信が破綻するため。50Hzならフライト丸ごとRAMに載る。
- 記録は単一スロット上書き（複数フライト追記は将来対応）。

### 再生の仕組み・安全策
- PLAYBACK入場では自動再生しない。**長押し or UART `p`** で開始。停止中はスロットル最小の安全アイドルを送出。
- 末尾まで再生したら自動停止。

### Arm操作（トグル式）
- ゲームパッドのボタンは押しボタン（保持不可）のため、**Aボタン押下エッジでArm(CH7)をトグル**（1回押す=Arm、もう1回=Disarm）。デバッグUARTに `# ARM`/`# DISARM` 出力。
- ⚠️ **TODO(実飛行前・方針決定済み)**: 誤タッチ1回で即Disarm=墜落するため、**GP3(pin5)の物理トグルスイッチに移行する**（スイッチ位置=Arm状態が目視可能）。実装は commit `1534582` に完成済み（デバウンス・切断後の再Armインヒビット・ドキュメント更新込み）で、スイッチ未購入のため revert 中（`eb4c241`）。**スイッチ入手後に `git revert eb4c241` で復活**させるだけでよい。

### フェイルセーフ
- ゲームパッド切断時はチャンネルを保持せず、**スロットル最小＋Arm(CH7)解除**を送出。Armトグルのラッチもリセット（再接続で勝手にArmが復活しない）。
  - TXモジュールはリンクを張り続けるため受信機側failsafeが効かない → 送信側で明示的に安全値を出す。

### 保留中の代替アーキテクチャ（二Pico構成）
制御Pico（USBホスト＋CRSF）と記録Pico（フラッシュ保存）を分離する案を検討済み。USB HIDは1ホスト専有のためY分岐不可で、「制御Pico→UART横流し→記録Pico」の形。制御のタイミング完全絶縁・墜落耐性が利点。現状は単一Picoで進行、UART出力を中立に保ち後から発展可能にする方針。

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
- コマンド入力: `d`=スナップショット, `s`=連続ストリームON/OFF, `p`=再生start/stop(PLAYBACK時), `?`=ヘルプ

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
