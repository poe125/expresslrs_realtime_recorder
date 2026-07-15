# ブレッドボード段階的ブリングアップ手順

ブレッドボードでサブシステムごとに切り分け検証し、最後にユニバーサル基盤へ移設する。
各 Stage は**前段が緑になってから**進むこと。**プロペラは終始外す**。

前提機材（本セッションで確定）:
- 給電: **安定化電源(PSU)**。電流制限を効かせ、Buck誤設定によるPico破壊を回避。LiPo+BuckはStage Fで導入。
- CRSF切り分け: **オシロスコープ**（GP0波形・反転極性・ボーレートを直接確認）。
- ゲームパッド: **汎用HIDパッド**（決め打ちパーサ。Stage Cで実機バイト配置を確認・調整見込み）。

詳細な単票チェックは `hardware-validation.md` を参照。本書はそれを段階順に割り当てたもの。

---

## Stage A — 電源のみ（過電圧でPico即死を防ぐ）

目的: Picoに正しい5.0Vだけが入ることを、Pico接続前に確定する。

1. [ ] PSU出力を **5.0V / 電流制限 ~0.7A** に設定し、**テスターで実測**。
2. [ ] （Buckを使う場合のみ）Buck OUT+ を**テスターで実測5.0V**に調整。可変品は初期12V等がある。
3. [ ] 極性確認: OUT+ → Pico **VBUS(pin40)**、OUT− → Pico **GND(pin3)**。**VSYSではない**。
4. [ ] Pico単体を接続（ゲームパッド・TX・LED未接続）。PSU電流が数十mA程度で安定（突入後）。
5. [ ] 内蔵LEDが点灯 or 点滅（ファーム次第。未書込なら消灯でOK）。異常発熱が無いこと。

> ✅ 合格条件: VBUSに5.0V±0.1、過大電流/発熱なし。

---

## Stage B — モードUI（ファーム＋UART＋LED＋ボタン、無線なし）

目的: ファームのUIロジックと GP2/GP10-12 配線を、ゲームパッド・TXなしで検証。

配線追加: GP4→USB-シリアル変換RXD（GND共通）、GP2→タクトSW→GND、GP10/11/12→LED→330Ω→GND。

1. [ ] 書込: ゲームパッド・外部5Vを外し、BOOTSEL押しながらmicro-USB接続 → `./scripts/flash.sh` → 配線復帰。
2. [ ] `./scripts/monitor.sh`（921600bps）で起動バナー・`Mode: SIMPLE`・`signal inversion: ON` を確認。
3. [ ] `?` でコマンド一覧が返る。
4. [ ] 起動直後 **青LED(GP12=SIMPLE)** 点灯。
5. [ ] 短押しごとに `# mode -> RECORD/PLAYBACK/SIMPLE`、LEDが **赤→緑→青** と移る。
6. [ ] PLAYBACK中に**長押し(0.6s)**で `# playback START/STOP`（データ無しでもトグルログが出る）。

> ✅ 合格条件: 3モード循環・LED対応・短長押し判定が全て一致。

---

## Stage C — ゲームパッド（USB Host / HIDパーサ確定）

目的: 汎用パッドの生HIDレポートから軸/ボタンのバイト配置を確定し、必要ならパーサ調整。TXはまだ繋がない。

配線追加: ゲームパッド ─OTG(micro-B→A)→ Pico micro-USBポート。（書込用ポートと共用のため、以降の書込は都度パッドを外す）

1. [ ] SIMPLEモードでパッドを操作しつつ `d`（スナップショット）。
2. [ ] `(1) HID raw` が全0でない。**VID/PID を控える**。
3. [ ] 各スティック/ボタンを個別に動かし、`(1)`のどのバイトが変化するか対応表を作る。
4. [ ] `(2) state` のスティック値、`(3) CRSF ch` が操作に追従（172〜1811、CH3=throttle最小で172付近）。
5. [ ] `(5) frame` 末尾 CRC が **OK**。
6. [ ] **ズレる場合**: `src/usb_gamepad.c` の `process_gamepad_report()` を実機バイト配置に合わせて調整・再ビルド・再書込。

> ✅ 合格条件: 全スティック/主要ボタンが期待するCRSF chに正しくマップされCRC OK。
> 記録: VID/PID、バイト配置、パーサ調整の有無を `hardware-validation.md` 記録テンプレへ。

---

## Stage D — TX疎通（CRSF反転切り分け・オシロ活用）

目的: Nano TX V2 経由で機体までRCを通す。反転/半二重/ボーレートを確定。**プロペラ無し**。

配線追加: Pico **GP0(pin1)** → Nano TX CRSF/S.Port信号線（参照図は680Ω直列）。TX VBAT → PSU 7〜13V（別チャンネル or 別電源。Buck5VとTX+は絶対に繋がない）。

1. [ ] **オシロでGP0を観測**: 送信バーストが出ているか、ボーレート≒921.6kbps、アイドルレベル（反転ON→アイドルLow）を確認。
2. [ ] 機体(Pavo Pico)受信機を対象TXにバインド済みにする。
3. [ ] SIMPLEで BetaflightのReceiverタブを開き、スティック/スイッチが追従するか確認。
4. [ ] **通らない場合の切り分け**（§5）:
   - オシロで極性が想定と逆なら `src/main.c` の `#define CRSF_INVERT_SIGNAL 0` に変更・再ビルド・再書込。
   - Nano TX側のシリアルプロトコル=CRSF設定も確認。
5. [ ] Arm(CH7=Aボタン)でモーターArm、放してDisarm（ペラ無し）。
6. [ ] **反転ON/OFFどちらで疎通したかを記録**し、CLAUDE.md/README/コードを実機結果に合わせる。

> ✅ 合格条件: Receiverタブで全ch追従、Arm/Disarm動作。反転極性を実機で確定。

---

## Stage E — 記録→再生・フェイルセーフ

目的: 記録フロー全体と切断時の安全動作を実機で確認。

1. [ ] RECORD(赤LED)で数十秒操作 → `# recording started`。
2. [ ] 短押しで抜ける → `# recording stopped, flushed N samples`（N>0）。
3. [ ] PLAYBACK(緑LED) → `# playback ready: N samples`。
4. [ ] **長押し**で再生開始 → Receiverタブで記録操作が再現。末尾で `# playback finished`。
5. [ ] 電源再投入後もPLAYBACKでNが残る（フラッシュ永続）。
6. [ ] フェイルセーフ: SIMPLEでArm状態にし**パッドを引き抜く** → 内蔵LED点滅・スロットル最小・Arm解除。再接続で復帰。

> ✅ 合格条件: 記録→再生の一巡再現、切断で安全値、再接続で復帰。

---

## Stage F — ユニバーサル基盤へ移設

目的: 検証済み回路を `docs/perfboard-layout.html` に従いハンダ実装し、短縮版で再検証。

1. [ ] perfboard-layout の netlist 通りにハンダ。GP0直列抵抗・半二重結線はStage Dで確定した方式で。
2. [ ] **給電をLiPo+Buckへ切替**: Buck出力を**テスターで実測5.0V**に調整してからPico接続（Stage A手順を再実施）。TX VBATはLiPo直結。GNDはLiPo−起点のスター結線。
3. [ ] 導通チェック（テスター）: 電源極性、GNDスター、GP0/GP2/GP4/GP10-12 の結線とショート無し。
4. [ ] 短縮再検証: Stage B(モードUI) → C(パッド追従1点) → D(疎通・Arm) → E(記録再生1回・フェイルセーフ) を各1パス。
5. [ ] 実運用メモ更新（Buck実測電圧、最終配線、既知の注意点）。

> ✅ 合格条件: 基板単体で Stage B〜E の短縮版が全て緑。
