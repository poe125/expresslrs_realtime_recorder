# ExpressLRS Realtime Recorder

## プロジェクト概要
USBゲームパッドをExpressLRS送信機として使用するアダプター。
ゲームパッドのUSB HID入力を取得し、CRSFプロトコルに変換してExpressLRS送信モジュールに送信する。

**2つのビルドターゲットがある**:
- **Pico版**（本来の想定構成）: Raspberry Pi PicoがUSBホスト。以下のドキュメントは主にこちら向け。
- **Pi4版**（`cmake -DBUILD_PI4=ON`）: ゲームパッドをRaspberry Pi 4のUSBポートに直接挿す構成。
  詳細は [Raspberry Pi 4版](#raspberry-pi-4版usb直結) を参照。CRSF/HIDパケット処理のロジック
  (`crsf.c` / `hid_parser.c` / `recorder.c` のRAM部分)はプラットフォーム非依存で両方から使う。

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
- **SDK**: Pico SDK (C/C++) — `/Users/akihito/pico-sdk`
- **ツールチェイン**: `/Users/akihito/gcc-arm-none-eabi`（**Homebrewの arm-none-eabi-gcc は `nosys.specs` を持たずリンクに失敗する**ので使わない）
- **ビルド**: CMake + Make

⚠️ `build/` で cmake を再実行するときは必ず両方のパスを渡すこと（キャッシュを消すと環境が失われる）:
```bash
PICO_SDK_PATH=/Users/akihito/pico-sdk PICO_TOOLCHAIN_PATH=/Users/akihito/gcc-arm-none-eabi cmake ..
```
※ SDK内 `hcd_rp2040.c` にF310用のローカル改変（DATA1ワークアラウンド）があり、SDKを入れ直すと失われる。

## Raspberry Pi 4版（USB直結）

ゲームパッドをPi4のUSBポートへ直接挿す構成。Pico版のCRSF/HIDパーサ/レコーダー
ロジック（`crsf.c` / `hid_parser.c` / `recorder.c`）はPico非依存のC言語なのでそのまま流用し、
USBホスト(TinyUSB)・UART・フラッシュ書込みだけをLinuxネイティブ実装に差し替えている
（`src/main.c` / `src/usb_gamepad.c` / `src/recorder.c` の `#elif defined(BUILD_PI4)` 部分）。

```
USBゲームパッド
    ↓ USB Host (hidraw, Pi4のUSBポートに直挿し)
Raspberry Pi 4
    └→ GPIO12(UART5 TX)/GPIO13(RX) (CRSF 921.6kbps, 要 外付け信号反転) → BetaFPV Nano TX Module V2
標準入力/出力（SSH等）でコマンド入力・ログ確認（d/s/p/i/m/?コマンドはPico版のUARTコマンドと同じ）
```

### ビルド・実行（Pi4本体上で直接ビルドする。クロスコンパイル不要）
```bash
mkdir build && cd build
cmake -DBUILD_PI4=ON ..
make
./expresslrs_realtime_recorder_pi4
```
- Ctrl+C (SIGINT/SIGTERM) で記録中なら保存してから終了する。
- 標準入力をcbreakモードにするため、SSH等の対話端末で実行すればEnter不要の単発キーで
  コマンド操作できる（`?`でヘルプ表示）。systemdサービス等TTYが無い実行では
  コマンド入力は単に無効になるだけで、CRSF送信自体は継続する。

### ボタン/LEDは無し（現状はコア機能のみ）
Pico版の物理ボタン(GP2/GP6)・LED(GP10-14)はPi4版には実装していない。
モード循環・入力プロファイル切替は標準入力コマンドで行う:

| コマンド | 動作 |
|---|---|
| `m` | モード循環 SIMPLE→RECORD→PLAYBACK→…（Pico版のGP2ボタン相当） |
| `i` | 入力プロファイル切替 GENERIC⇔LITERADIO（Pico版のGP6ボタン相当） |
| `p` | PLAYBACK中の再生start/stop |
| `d` | 次フレームの全段スナップショット |
| `s` | 連続ストリームON/OFF |
| `?` | ヘルプ |

必要になれば物理ボタン/LEDをlibgpiod等で追加することは可能（今回は見送り）。

### CRSF UART: GPIO12/13 (UART5) — 信号反転には外付け回路が必要
- `/dev/ttyAMA5`（`config.txt` の `dtoverlay=uart5` でGPIO12=TX/13=RX/14=CTS/15=RTSが有効化される）
  を921600bps 8N1で使う。
- ⚠️ **`dtoverlay=uart5,txd5_invert` は効いていない**: `txd5_invert` は公式`uart5`オーバーレイの
  パラメータとして存在しない（`/boot/firmware/overlays/README`・`uart5.dtbo` を確認済み、
  受理される param は `ctsrts`/`rs485`系のみ）。起動はするが信号反転は行われていない。
- BCM2711のPL011は、RP2040のGPIOオーバーライド（`gpio_set_outover`/`gpio_set_inover`）に相当する
  ハードウェア信号反転機能を持たない。よって **Nano TX Module V2の反転S.Port信号に合わせるには
  GPIO12/13とモジュールの間に外付けの信号反転回路が必須**（ソフトウェアだけでは解決できない）。
  - 標準的な解決策: NPNトランジスタ1〜2個によるFrSky S.Port用インバータ回路
    （FPVコミュニティで広く使われる定番回路。TX→ベース、コレクタをプルアップ抵抗経由で
    バス、という構成でTXを反転しつつ半二重バスに合流させる）。市販の「S.Port/FrSkyインバータ
    モジュール」を使ってもよい。
  - 配線・実装が済むまでは、`init_crsf_uart()` はUARTのオープンにさえ失敗しなければ動作し続ける
    （CRSF出力は「信号が正しく反転されていないだけ」の状態で送出される）。実機に繋ぐ前に
    テスターやロジックアナライザで反転が機能しているか必ず確認すること。
- 半二重テレメトリドレイン（`crsf_drain_telemetry()`）で **`tcdrain()`（`TCSBRK` ioctl）を意図的に
  呼んでいない**: 実測でこの機体のPL011ドライバは`tcdrain()`1回に約8ms かかり
  （Picoの`uart_tx_wait_blocking()`のようなレジスタ直読みの即時ポーリングとは違い、
  ジフィー単位のポーリング実装らしい）、500Hz送信の2ms予算を大きく超えて詰まる。
  26バイト@921600bpsの送信自体は約280usで終わるため、後続の最大300usアイドル検出ループの
  中で自然に完了を待つ設計にしている。今後Linuxカーネル/ドライバ側の挙動が変わった場合は
  再測定すること。

### USBゲームパッド: hidraw経由
- 起動時に `/dev/hidraw0`〜`/dev/hidraw15` を走査し、VID/PID専用パーサ対象（DS4/F310）か
  ディスクリプタ解析で軸が見つかったデバイスを最初の1つ選んで使う。
  環境変数 `GAMEPAD_HIDRAW=/dev/hidrawN` で明示指定も可能（複数HID機器がある場合用）。
- 切断は検知して自動的に再走査する（フェイルセーフ値の送出はPico版と同じ）。
- ⚠️ **hidrawは既定でrootのみアクセス可能**。一般ユーザで使うには udev ルールが必要
  （本プロジェクトでは `/etc/udev/rules.d/99-expresslrs-hidraw.rules` に
  `KERNEL=="hidraw*", SUBSYSTEM=="hidraw", MODE="0660", GROUP="plugdev"` を追加し、
  実行ユーザを `plugdev` グループに入れて解決済み）。新しいPi4環境で使う場合は同様の設定が要る。
- 実機確認済み(2026-09-02): Logitech F310 (VID 046D/PID C216) をdrone-dev01のUSBポートに直挿しし、
  VID/PID専用パーサで正しく認識、CRSF送信ループが実測でも安定して500Hz(2.00ms周期)で回ることを確認。

### 記録データの保存先（フラッシュの代わりにファイル）
- 既定: `~/.expresslrs_recorder/flight.log`（ヘッダ+パック済みpayloadの連結。Pico版のフラッシュ
  ログと同じフォーマット）。環境変数 `RECORDER_LOG_PATH` で保存先を上書き可能。
- RECORDを抜けた時点でRAMバッファをまとめてファイルへ書き出す（Pico版と同じタイミング）。

## ピン配置
| Picoピン | 機能 | 接続先 |
|----------|------|--------|
| USB (micro-USB) | USB Host | ゲームパッド（OTG変換アダプタ経由） |
| GP0 (pin1) | UART0 TX | Nano TX S.Port/CRSF信号 (921.6kbps, 反転, 半二重) |
| GP1 (pin2) | UART0 RX | (半二重テレメトリ受信) |
| GP4 (pin6) | UART1 TX | PC デバッグ/ログ (921600bps, USB-シリアル変換経由) |
| GP5 (pin7) | UART1 RX | PC (コマンド入力) |
| GP2 (pin4) | モード切替ボタン | タクトSW → GND（内部プルアップ, 押下=Low） |
| GP6 (pin9) | 入力切替ボタン | タクトSW → GND（GND=pin8が隣） |
| GP10 (pin14) | モードLED 赤 | 記録(RECORD)点灯 (100Ω → GND) |
| GP11 (pin15) | モードLED 緑 | 再生(PLAYBACK)点灯 (100Ω → GND) |
| GP12 (pin16) | モードLED 青 | 単純(SIMPLE)点灯 (100Ω → GND) |
| GP13 (pin17) | 入力LED 汎用 | GENERIC(積算)点灯 (100Ω → GND=pin18) |
| GP14 (pin19) | 入力LED LiteRadio | LITERADIO(絶対値)点灯 (100Ω → GND=pin18) |
| VBUS (pin40) | 電源入力 | 降圧モジュール出力5V（LiPoから降圧、Pico＋ゲームパッド給電） |
| GND (pin3 等) | グランド | 共通GND（下記参照） |
| 内蔵LED | 状態表示 | 接続時:点灯 / 未接続:点滅 / 再生モード:速点滅 |

※GP2/GP10-12 の配置は参照図（`references/240529_104.jpg`, 63bit氏 R63b）に倣った空きGPIO。
※GP6/GP13/GP14 はGNDピンが隣接する空きGPIOから選定。**GP15(pin20)は本個体にヘッダ未実装のため使用不可**。GP3はArmスイッチ用に予約。
※LEDの直列抵抗は**実機で100Ω**（参照図は330Ωだが、緑/青LEDは順方向電圧が3V前後あり330Ωでは暗いため100Ωを採用）。5個とも同じ値にして明るさを揃える。

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

## 入力プロファイル（汎用ゲームパッド / LiteRadio3）
スティックが**自動中央復帰する汎用ゲームパッド**と、**位置が保持されるLiteRadio3のジンバル**では
スロットルの解釈が異なるため、GP6のボタンで切り替え、GP13/GP14のLEDで現在の設定を表示する。
起動時は手持ちの**GENERIC**。UARTコマンド `i` でも切替可能。

| プロファイル | LED | スロットル(CH3)の扱い |
|---|---|---|
| GENERIC (汎用) | GP13 | **積算**: 左スティックの倒し量=増減の速さ。中央=現在値を保持 |
| LITERADIO | GP14 | **絶対値**: ジンバル位置をそのままスロットルに（従来動作） |

### 積算スロットルの仕様（GENERIC時）
- フルデフレクションで最小→最大まで **2.0秒**（`THROTTLE_FULL_TRAVEL_MS`）。倒し量に比例して増減速度が変わる。
- 中央±約9%は**不感帯**（`THROTTLE_DEADZONE`）。スティックのジッタでスロットルが勝手に流れるのを防ぐ。
- **積算値が最小にリセットされる条件**: Disarm時 / ゲームパッド切断時 / プロファイル切替時。前回のスロットルを引き継いだまま再Armして急に回り出すのを防ぐ。
- フラッシュ書込み等でループが止まっても飛ばないよう、1フレームで進める時間に上限（20ms）を設けている。

### HIDレポート解析（未知のゲームパッド対応）
LiteRadio 3 のような**手元に無いデバイスにもバイト位置決め打ちでは対応できない**ため、
`src/hid_parser.c` で**HIDレポートディスクリプタを解析**し、軸・ボタンのビット位置を
自動で求める。全てのHIDデバイスがディスクリプタを自己申告するので、原理的にどの
ゲームパッドでも読める。解析の優先順位は:

1. **VID/PID専用パーサ**（DualShock4 / F310）— 実機検証済みなので最優先
2. **ディスクリプタ解析**（`hid_parser.c`）— 未知の機種はここ。**LiteRadio 3 もこの経路**
3. バイト位置決め打ち（ディスクリプタが読めない機種の最終手段）

軸のマッピングは Generic Desktop の Usage から決める:
`X→LX, Y→LY, Z→RX, Rz→RY, Rx→L2, Ry→R2`。
ただし **Z/Rz が無く Rx/Ry がある機種（ラジオ送信機に多い）では Rx→RX, Ry→RY** に割り当てる。

接続時にデバッグUARTへ **解析結果（各軸のビット位置・サイズ・論理範囲）を出力**するので、
実機での答え合わせができる。想定と違えば `assign_axes()` を直す。

### LiteRadio 3（実機確認済み 2026-08-26）
**VID 0483 / PID 572B**（STMicroelectronics）。16bit×8ch（論理範囲 0..2047）＋ボタン、
レポート長18バイト、ハット無し。ディスクリプタ順は **X, Y, Z, Rx, Ry, Rz, Slider, (もう1つ)**。

送信機は**スティックではなくAETRのチャンネル順**で軸を並べるため、ゲームパッドとしての
スティック解釈（X=左スティックX…）を当てるとバラバラになる。そこで `hid_layout_t.channels[]`
に**ディスクリプタ順のまま**の値も保持し、LITERADIOプロファイル時は
`map_radio_to_channels()` が CH1〜 へ**素通し**する（積算もスティック読み替えもしない）。

実測で確定した対応（スティック→CH、極性込み）:

| スティック | Usage | CRSF | 実測 |
|---|---|---|---|
| 右 左右 | X | CH1 Roll | 左=172 / 右=1811 |
| 右 上下 | Y | CH2 Pitch | 下=182 / 上=1803 |
| 左 上下 | Z | CH3 Throttle | 下=172 / 上=1811 |
| 左 左右 | Rx | CH4 Yaw | 左=172 / 右=1811 |

Ry/Rz/Slider は CH5/CH6 へ。送信機のスイッチは**ボタン**として届き、CH7〜CH12 に割り当てられる。

> ⚠️ **LiteRadio 3 使用時は必ず GP6 で LITERADIO プロファイルに切り替える**こと。
> GENERIC のままだとゲームパッド解釈になり軸がバラバラになる。
> ⚠️ **スロットルのジンバルは位置を保持する**（自動で下がらない）。Arm前に必ず一番下に
> あることを目視確認する。実測でも操作の弾みでスロットルが62%相当のまま残っていた。

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
- コマンド入力: `d`=スナップショット, `s`=連続ストリームON/OFF, `p`=再生start/stop(PLAYBACK時), `i`=入力プロファイル切替, `?`=ヘルプ

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
