# 実機検証手順（ブリングアップ・チェックリスト）

制御経路（ゲームパッド → Pico → Nano TX V2 → 機体）と、記録/再生・フェイルセーフを
実機で確認するための手順。**プロペラを外した状態で**行うこと。

---

## 0. 事前準備：配線チェック（通電前）

テスターで**通電前に**以下を確認する。特に電源の極性・電圧を最優先。

### 電源（最重要）
- [ ] Buck出力を **テスターで実測5.0V** に調整済み（可変モジュールは初期12V等がある）。Picoに繋ぐ前に必ず確認。
- [ ] Buck OUT+ → Pico **VBUS(pin40)**（VSYSではない）。
- [ ] Nano TX VBAT → **LiPo直結(7〜13V)**。**Buck5VとLiPo+は絶対に繋がない**。
- [ ] GND スター結線：LiPo− を起点に、Nano TX GND / Buck GND / Pico GND(pin3) を集約。

### 信号
- [ ] Pico **GP0(pin1)** → Nano TX の CRSF/S.Port 信号線。
- [ ] （参照図では TX 線に 680Ω 直列、TXD/RXD を1本に結線。半二重の配線方式は §5 で切り分け）
- [ ] Pico **GP4(pin6)** → USB-シリアル変換 RXD、変換 GND → 共通GND。

### ボタン・LED（新規）
- [ ] **GP2(pin4)** → タクトSW → GND（内部プルアップ使用、外付け抵抗不要）。
- [ ] **GP3(pin5)** → Arm用トグルSW → GND（内部プルアップ使用。ON=Arm）。
- [ ] **GP10/GP11/GP12** → 各 LED アノード、LED カソード → 330Ω → GND（赤/緑/青）。

---

## 1. ファームウェア書き込み

micro-USB はゲームパッド専有のため、書き込み時のみ手順が特殊。

1. [ ] ゲームパッドと外部5Vを外す。
2. [ ] BOOTSEL を押しながら Pico を PC に micro-USB 接続。`RPI-RP2` がマウントされる。
3. [ ] `./scripts/flash.sh` を実行。
4. [ ] 書き込み後、配線を元に戻す（ゲームパッド接続・外部給電）。

---

## 2. 起動・デバッグUART確認

1. [ ] USB-シリアル変換を PC に接続し、`./scripts/monitor.sh`（921600bps）。
2. [ ] 起動バナーと `Mode: SIMPLE ...`、`CRSF UART initialized...`、`signal inversion: ON` が出る。
3. [ ] `?` を送るとコマンド一覧が返る。

---

## 3. モード切替（ボタン＋LED）

1. [ ] 起動直後は **青LED(GP12=SIMPLE)** 点灯。
2. [ ] ボタン短押しごとに `# mode -> RECORD/PLAYBACK/SIMPLE` が出て、**赤→緑→青**とLEDが移る。
3. [ ] PLAYBACK に入ると `# playback ready: N samples in flash` が出る（初回は N=0）。

---

## 4. 制御経路の値検証（スナップショット）

SIMPLE モードで、ゲームパッドを操作しながら `d` を送る。

- [ ] `(1) HID raw` に生レポートが出る（全0でない）。VID/PID を控える。
- [ ] `(2) state` のスティック値がゲームパッド操作に追従する。
- [ ] `(3) CRSF ch` が 172〜1811 の範囲で動く。CH3(Throttle) はスロットル最小で172付近。
- [ ] `(5) frame` の末尾 CRC が **OK** 表示。
- [ ] 汎用パーサがズレる場合（値が動かない/軸が違う）は §6 のHID対応を検討。

---

## 5. ⚠️ CRSF反転の切り分け（最重要の疑い）

現コードは `CRSF_INVERT_SIGNAL=1`（S.Port前提の反転）。だが **ELRS TXモジュールへのCRSFは
通常「非反転」**で、参照図(63bit氏 R63b)も反転記載なし・680Ω直列のみ。ここが逆だと機体まで通らない。

1. [ ] まず現状（反転ON）で機体がバインド済みのTXにRCが通るか確認（後述 §7）。
2. [ ] 通らなければ `src/main.c` の `#define CRSF_INVERT_SIGNAL 0` に変更・再ビルド・書込みして再試験。
3. [ ] どちらで通るか記録し、CLAUDE.md/README を実機結果に合わせて更新する。

> Nano TX V2 側の Telemetry/シリアルプロトコル設定（CRSF）も合わせて確認。

---

## 6. HIDパーサの実機対応（必要時）

汎用パーサは `report[0]=LX, [1]=LY, [2]=RX, [3]=RY, [4..]=buttons` を仮定（HIDディスクリプタ未解析）。
§4 の `(1) HID raw` と実操作の対応から、実機パッドのバイト配置を確認し、必要なら
`src/usb_gamepad.c` の `process_gamepad_report()` を実機に合わせて調整する。
（DualShock4 は専用パーサあり: VID 0x054C / PID 0x09CC or 0x05C4）

---

## 7. 機体までの疎通（プロペラを外して）

1. [ ] 機体(Pavo Pico)の受信機を対象TXにバインド済みにする。
2. [ ] SIMPLE で、Betaflight等のReceiverタブでスティック/スイッチが追従するか確認。
3. [ ] Arm(CH7)は**GP3の物理トグルスイッチ**: ONでArm、OFFでDisarm（プロペラ無し）。
   - ※Betaflight Configurator接続中は MSP/CLI フラグでArm不可（ビープで拒否通知）。USBを抜いて確認する。
   - ※ゲームパッド切断後はスイッチを一度OFFに戻すまで再Armしない（インヒビット）。

---

## 8. 記録 → 再生

1. [ ] RECORD に入れて（赤LED）数十秒操作 → `# recording started`。
2. [ ] 次のモードへ短押しで抜ける → `# recording stopped, flushed N samples`（N>0）。
3. [ ] PLAYBACK に入る（緑LED）→ `# playback ready: N samples`。
4. [ ] **長押し(0.6s)** で再生開始 → `# playback START`。Receiverタブで記録した操作が再現される。
5. [ ] 末尾で `# playback finished`、または長押しで停止。
6. [ ] 電源を入れ直しても PLAYBACK で N が残る（フラッシュ永続）ことを確認。

---

## 9. フェイルセーフ

1. [ ] SIMPLE で Arm 状態（プロペラ無し）にし、**ゲームパッドを引き抜く**。
2. [ ] 内蔵LEDが点滅に変わり、スロットル最小＋Arm解除（Disarm）になることを Receiverタブ/挙動で確認。
3. [ ] 再接続で復帰することを確認。

---

## 記録テンプレ

| 項目 | 結果 | メモ |
|------|------|------|
| Buck実測電圧 | 未 | Stage F で導入。Stage A-C は PSU 直(5.0V実測/20mA) |
| 反転 ON/OFF どちらで疎通 | 未 | §5 / Stage D。オシロでGP4のビット幅も測ること（[breadboard-bringup.md](breadboard-bringup.md) Stage B の既知の問題） |
| ゲームパッド VID/PID | **046D:C216** (F310 / Dモード) | Xモードは 046D:C21D で**HIDではない**ので必ず裏スイッチを **D** に |
| HIDパーサ調整要否 | **要（ボタンのみ）** | §6 / 軸 `report[0..3]` は現行のままで正解。ボタンは `((report[4]>>4)|(report[5]<<4)) & 0x0FFF`、ハットは `report[4]&0x0F`。配置は breadboard-bringup.md Stage C 参照 |
| 記録→再生 サンプル数 | 137 (2026-07-16, ベンチ) | パッド無しでのStage B確認時。50Hzで2.74秒分、レート正常 |
| フェイルセーフ動作 | 未 | Stage E。パッド未接続時に CH3=172 になることは確認済み |

> ⚠️ **未解決 (2026-07-17)**: F310(Dモード)は列挙もHIDマウントも成功するが、**割り込みINレポートを1本も返さない**。
> マウス/キーボード(いずれもLow Speed)は正常動作するため、ホストスタックは白。詳細と次の一手は
> [breadboard-bringup.md](breadboard-bringup.md) の Stage C 実施記録を参照。
