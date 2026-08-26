#include <stdio.h>
#include <string.h>
#include "crsf.h"
#include "usb_gamepad.h"
#include "recorder.h"

#ifdef PICO_BOARD
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "tusb.h"

// ========================================
// ピン設定
// ========================================

// CRSF出力用UART（Nano TX Moduleへ）
#define CRSF_UART       uart0
#define CRSF_UART_TX    0   // GP0
#define CRSF_UART_RX    1   // GP1 (半二重テレメトリ受信に使用)
// ELRS V3.x の BetaFPV Nano TX Module V2 は 921600bps
// （旧来のレシーバ直結用 420000 ではなくモジュール用の高速ボーレート）
#define CRSF_BAUD_RATE  921600

// Nano TX V2 の S.Port は反転UART。Pico側でTX/RXを反転する。
// （senderでは Pi の dtoverlay で反転していたのと同等の処理）
#define CRSF_INVERT_SIGNAL  1

// デバッグ用UART
// stdio(printf/getchar) は CMake の PICO_DEFAULT_UART=1 設定により
// uart1 = GP4(TX)/GP5(RX) @921600bps にルーティングされる。
#define DEBUG_UART      uart1
#define DEBUG_UART_TX   4   // GP4
#define DEBUG_UART_RX   5   // GP5
#define DEBUG_BAUD_RATE 921600

// 連続ストリーム(C)の間引き: 500Hz / 20 = 25Hz で出力
#define DEBUG_STREAM_DECIMATION 20

// LED（内蔵: 接続/再生ステータス表示用）
#define LED_PIN         PICO_DEFAULT_LED_PIN

// ========================================
// モード切替 (ボタン + 外付けLED)
// ========================================
// 参照図(63bit氏 R63b)の空きGPIO配置に合わせる。
// ボタン: GP2 にタクトSW（内部プルアップ, 押下=Low）。押すたびにモード循環。
#define MODE_BTN_PIN     2
// モード表示LED（各 330Ω 直列で GND へ）
#define LED_RECORD_PIN   10  // 赤: 記録
#define LED_PLAYBACK_PIN 11  // 緑: 再生
#define LED_SIMPLE_PIN   12  // 青: 単純
// ボタンのデバウンス時間
#define MODE_BTN_DEBOUNCE_MS 30
// 長押し判定のしきい値（これ以上保持で「長押し」= 再生start/stop）
#define MODE_BTN_LONG_MS     600

// ========================================
// 入力プロファイル切替 (ボタン + LED2つ)
// ========================================
// スティックが自動中央復帰する汎用ゲームパッドと、位置が保持される
// LiteRadio3のジンバルでは、スロットルの解釈を変える必要がある。
// GP6のボタンで切り替え、GP13/GP14のLEDで現在のプロファイルを表示する。
// ピン選定: GP6はpin9でGND(pin8)が隣。GP13(pin17)/GP14(pin19)はGND(pin18)を
// 挟んで両隣なのでLED2つのカソードを1点にまとめられる。
// （GP3はArmスイッチ用に予約。GP15=pin20は本個体にヘッダ未実装のため使わない）
#define INPUT_BTN_PIN            6
#define LED_INPUT_GENERIC_PIN    13  // 汎用ゲームパッド（スロットル積算）
#define LED_INPUT_LITERADIO_PIN  14  // LiteRadio3（スロットル絶対値）
#define INPUT_BTN_DEBOUNCE_MS    30

// スロットル積算のパラメータ（INPUT_GENERIC時のみ有効）
// フルデフレクションでスロットルが最小→最大まで動くのに要する時間。
// 短いほど機敏だが荒くなる。
#define THROTTLE_FULL_TRAVEL_MS  2000
// 中央付近の不感帯（軸のフルスケール32767に対する値）。
// スティックのジッタでスロットルが勝手に流れるのを防ぐ。
#define THROTTLE_DEADZONE        3000
// 1フレームで進める最大時間。フラッシュ書込み等でループが止まった直後に
// スロットルが一気に飛ぶのを防ぐ。
#define THROTTLE_MAX_DT_MS       20

// 入力プロファイル（ボタン押下で GENERIC ⇔ LITERADIO をトグル）
typedef enum {
    INPUT_GENERIC = 0,   // 汎用ゲームパッド: 左スティック上下=スロットルの増減率
    INPUT_LITERADIO,     // LiteRadio3: 左スティック位置=スロットル絶対値
    INPUT_PROFILE_COUNT
} input_profile_t;

// 起動時は手持ちの汎用ゲームパッド。LiteRadioは接続時に手動で切り替える。
static input_profile_t current_input = INPUT_GENERIC;

// 入力プロファイル切替ボタンの状態（デバウンス用）
static bool     ibtn_pressed_state = false;
static uint32_t ibtn_last_change_ms = 0;

// スロットル積算値（CRSF単位×1000のミリ単位で保持し、丸め誤差の蓄積を防ぐ）
static int32_t  throttle_accum_milli = (int32_t)CRSF_CHANNEL_MIN * 1000;
static uint32_t throttle_last_update_ms = 0;

// 実体はLED制御と並べて後述。UARTコマンド 'i' からも呼ぶため先に宣言する。
static void input_profile_toggle(void);
static const char* input_name(input_profile_t p);

// 動作モード（押下で SIMPLE→RECORD→PLAYBACK→SIMPLE… と循環）
typedef enum {
    MODE_SIMPLE = 0,   // 単純: gamepad → CRSF（現行のパススルー）
    MODE_RECORD,       // 記録: gamepad → CRSF ＋ ログ保存（Phase2で実装）
    MODE_PLAYBACK,     // 再生: フラッシュ → CRSF（Phase3で実装）
    MODE_COUNT
} op_mode_t;

// 起動時は最も安全な単純パススルー。誤って再生で機体を駆動しないため。
static op_mode_t current_mode = MODE_SIMPLE;

// ボタン状態（デバウンス＋短押し/長押し判別用）
static bool     btn_pressed_state = false;   // 確定済みの押下状態
static uint32_t btn_last_change_ms = 0;
static uint32_t btn_press_start_ms = 0;      // 押下開始時刻
static bool     btn_long_fired = false;      // 今回の押下で長押しアクション済みか

// 再生状態（PLAYBACKモードで長押し or 'p' コマンドにより開始/停止）
static bool     playback_running = false;
static uint32_t pb_sample_idx = 0;   // 次に送出するフラッシュサンプル番号
static uint16_t pb_frame_ctr = 0;    // 50Hz送出のための500Hzフレームカウンタ

// 再生の開始/停止をトグル（PLAYBACKモードでのみ有効）。
// long-press と 'p' コマンドの両方から使う共通処理。
static void playback_toggle(void) {
    if (current_mode != MODE_PLAYBACK) {
        printf("# playback toggle ignored (not in PLAYBACK mode)\n");
        return;
    }
    playback_running = !playback_running;
    if (playback_running) { pb_sample_idx = 0; pb_frame_ctr = 0; }
    printf("# playback %s (%u samples)\n",
           playback_running ? "START" : "STOP",
           (unsigned)recorder_flash_sample_count());
}

// ========================================
// CRSF送信設定
// ========================================

// CRSF送信間隔（ミリ秒）
// ExpressLRS 500Hz = 2ms間隔
#define CRSF_SEND_INTERVAL_MS  2  // 500Hz（senderと同じ。ELRS V3.x標準）

// ========================================
// グローバル変数
// ========================================

// CRSFチャンネル値
static uint16_t crsf_channels[CRSF_NUM_CHANNELS];

// 最後のCRSF送信時刻
static uint32_t last_crsf_send_time = 0;

// ログカウンタ
static uint32_t log_counter = 0;

// ========================================
// デバッグ/値確認の状態
// ========================================

// 'd' コマンドで次フレームの全段スナップショットを要求
static bool dbg_snapshot_request = false;
// 's' コマンドで連続ストリーム(C)をON/OFF（デフォルトOFF）
static bool dbg_stream_enabled = false;
// ストリーム間引きカウンタ
static uint16_t dbg_stream_counter = 0;
// 直近のドレインで読み捨てたテレメトリ（スナップショット(6)用）
static uint8_t dbg_drain_buf[32];
static uint16_t dbg_drain_len = 0;

// ========================================
// チャンネルマッピング
// ========================================

// 軸の上下反転。-INT16_MIN は int16 に収まらず -32768 のまま化けるため飽和させる
static inline int16_t axis_invert(int16_t v) {
    return (v == INT16_MIN) ? INT16_MAX : (int16_t)-v;
}

// Arm(CH7)のトグルラッチ。ゲームパッドのAは押しボタン（モーメンタリ）で
// 押し続けられないため、押下エッジごとにArm/Disarmを切り替える。
static bool arm_latched = false;
static uint16_t prev_buttons = 0;

// スロットル積算値を最小にリセットする。
// Disarm時・切断時・プロファイル切替時に呼び、直前のスロットルが
// 意図せず引き継がれないようにする。
static void throttle_reset(void) {
    throttle_accum_milli = (int32_t)CRSF_CHANNEL_MIN * 1000;
}

// 汎用ゲームパッド用のスロットル積算。
// 自動中央復帰するスティックでは位置をそのままスロットルにできないため、
// スティックの倒し量を「増減の速さ」として積分する（中央=現在値を保持）。
// 戻り値: CRSF単位のスロットル値。
static uint16_t throttle_accumulate(int16_t stick_up, uint32_t now) {
    // 経過時間。初回およびループ停止直後は上限で頭打ちにする。
    uint32_t dt = now - throttle_last_update_ms;
    throttle_last_update_ms = now;
    if (dt > THROTTLE_MAX_DT_MS) dt = THROTTLE_MAX_DT_MS;

    // 不感帯を差し引き、残りを 0〜32767 に引き伸ばす
    int32_t mag = stick_up < 0 ? -(int32_t)stick_up : (int32_t)stick_up;
    if (mag > THROTTLE_DEADZONE) {
        mag = (mag - THROTTLE_DEADZONE) * 32767 / (32767 - THROTTLE_DEADZONE);
        int32_t range_milli = (int32_t)(CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN) * 1000;
        // delta = 全可動域 × (倒し量/フルスケール) × (dt/フル移動時間)
        int32_t delta = (int32_t)((int64_t)range_milli * mag * dt
                                  / ((int64_t)32767 * THROTTLE_FULL_TRAVEL_MS));
        throttle_accum_milli += (stick_up < 0) ? -delta : delta;

        if (throttle_accum_milli < (int32_t)CRSF_CHANNEL_MIN * 1000) {
            throttle_accum_milli = (int32_t)CRSF_CHANNEL_MIN * 1000;
        } else if (throttle_accum_milli > (int32_t)CRSF_CHANNEL_MAX * 1000) {
            throttle_accum_milli = (int32_t)CRSF_CHANNEL_MAX * 1000;
        }
    }

    return (uint16_t)(throttle_accum_milli / 1000);
}

// ゲームパッド入力をCRSFチャンネルにマッピング
static void map_gamepad_to_channels(const gamepad_state_t *gamepad, uint32_t now) {
    // 標準的なドローン操縦マッピング (Mode 2)
    // CH1: Roll     (右スティック X)
    // CH2: Pitch    (右スティック Y) ※反転
    // CH3: Throttle (左スティック Y) ※反転
    // CH4: Yaw      (左スティック X)

    crsf_channels[0] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_RX]);  // Roll
    crsf_channels[1] = crsf_normalize_channel(axis_invert(gamepad->axes[GAMEPAD_AXIS_RY])); // Pitch (反転)
    crsf_channels[3] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_LX]);  // Yaw

    // CH3 Throttle: 入力プロファイルで解釈が変わる
    int16_t stick_up = axis_invert(gamepad->axes[GAMEPAD_AXIS_LY]);  // 上=正
    if (current_input == INPUT_GENERIC) {
        // 自動中央復帰スティック: 倒し量を増減率として積算（中央=保持）
        crsf_channels[2] = throttle_accumulate(stick_up, now);
    } else {
        // LiteRadio3: ジンバル位置がそのままスロットル
        crsf_channels[2] = crsf_normalize_channel(stick_up);
    }

    // CH5-8: トリガーとボタン
    // L2トリガー → CH5
    crsf_channels[4] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_L2]);
    // R2トリガー → CH6
    crsf_channels[5] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_R2]);

    // ボタンをスイッチチャンネルにマッピング
    // A/Crossボタン → CH7 (Arm): 押下エッジでトグル
    if ((gamepad->buttons & GAMEPAD_BTN_A) && !(prev_buttons & GAMEPAD_BTN_A)) {
        arm_latched = !arm_latched;
        // Disarmしたら積算スロットルも最小に戻す。次のArmで前回の
        // スロットルのまま回り出すのを防ぐ。
        if (!arm_latched) throttle_reset();
        printf("# %s\n", arm_latched ? "ARM" : "DISARM");
    }
    prev_buttons = gamepad->buttons;
    crsf_channels[6] = arm_latched ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    // B/Circleボタン → CH8
    crsf_channels[7] = (gamepad->buttons & GAMEPAD_BTN_B) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;

    // CH9-12: その他のボタン
    crsf_channels[8]  = (gamepad->buttons & GAMEPAD_BTN_X) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[9]  = (gamepad->buttons & GAMEPAD_BTN_Y) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[10] = (gamepad->buttons & GAMEPAD_BTN_LB) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[11] = (gamepad->buttons & GAMEPAD_BTN_RB) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;

    // CH13-16: 未使用（中央値）
    crsf_channels[12] = CRSF_CHANNEL_MID;
    crsf_channels[13] = CRSF_CHANNEL_MID;
    crsf_channels[14] = CRSF_CHANNEL_MID;
    crsf_channels[15] = CRSF_CHANNEL_MID;
}

// 入力喪失時（ゲームパッド切断）に送るフェイルセーフ値。
// TXモジュールはリンクを張り続けるため受信機側のfailsafeは効かない。
// こちらで明示的に安全値（スロットル最小・Arm解除・スティック中央）を送る。
static void set_failsafe_channels(void) {
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        crsf_channels[i] = CRSF_CHANNEL_MID;
    }
    crsf_channels[2] = CRSF_CHANNEL_MIN;  // CH3 Throttle 最小
    crsf_channels[6] = CRSF_CHANNEL_MIN;  // CH7 Arm 解除（最重要: モーター停止）
    // Armトグルもリセット（再接続した瞬間に勝手にArmが復活しないように）
    arm_latched = false;
    prev_buttons = 0;
    // 積算スロットルもリセット（再接続で前回のスロットルが復活しないように）
    throttle_reset();
}

// ========================================
// UART送信
// ========================================

static void crsf_uart_send(const uint8_t *data, size_t len) {
    uart_write_blocking(CRSF_UART, data, len);
}

// 半二重テレメトリドレイン
// Nano TX V2 は S.Port 1本の半二重通信。RCフレーム送信後に
// モジュールが返すテレメトリを読み捨てないと、次回送信時に
// バス衝突が起き ch5〜ch16 が化ける（senderのdrainTelemetry相当）。
static void crsf_drain_telemetry(void) {
    // 送信完了をハードウェア的に保証（最後のビットが線上に出るまで待つ）
    uart_tx_wait_blocking(CRSF_UART);

    // バス上のバイトを読み捨てる。半二重では自分の送信エコーもRX FIFOに
    // 入るため必ず排出する必要がある（放置するとFIFOが溢れる）。
    // 500Hz(2ms)の送信周期を崩さないよう、ドレイン総時間に上限を設ける。
    dbg_drain_len = 0;  // スナップショット用に読み捨てたバイトを記録
    absolute_time_t deadline = make_timeout_time_us(300);  // 最大300us
    int idle_us = 0;
    while (idle_us < 60) {  // 連続60usアイドルで応答途切れと判断
        if (uart_is_readable(CRSF_UART)) {
            uint8_t b = uart_getc(CRSF_UART);
            if (dbg_drain_len < sizeof(dbg_drain_buf)) {
                dbg_drain_buf[dbg_drain_len++] = b;
            }
            idle_us = 0;
        } else {
            if (time_reached(deadline)) {
                break;
            }
            busy_wait_us(10);
            idle_us += 10;
        }
    }
}

// ========================================
// デバッグログ
// ========================================

static void debug_log_channels(void) {
    // ストリーム有効時はヒートビートを抑制（ビューア出力を汚さない）
    if (dbg_stream_enabled) return;

    // 1秒ごとにログ出力
    log_counter++;
    if (log_counter >= (1000 / CRSF_SEND_INTERVAL_MS)) {
        log_counter = 0;

        const gamepad_state_t *gp = usb_gamepad_get_state();

        printf("[%s] CH1-4: %4d %4d %4d %4d | BTN: 0x%04X\n",
               gp->connected ? "CONN" : "----",
               crsf_channels[0], crsf_channels[1],
               crsf_channels[2], crsf_channels[3],
               gp->buttons);
    }
}

// ========================================
// 値確認: スナップショット(A) / ストリーム(C) / コマンド
// ========================================

// (A) 1フレームの全処理ポイントをHEX付きでダンプ
//   ① 生HIDレポート → ② デコード後state → ③ CRSFチャンネル
//   → ④ パック後payload → ⑤ フレーム全体+CRC検証 → ⑥ ドレインしたテレメトリ
static void dbg_dump_snapshot(const uint8_t *frame, size_t frame_len) {
    const gamepad_state_t *gp = usb_gamepad_get_state();
    const uint8_t *raw = NULL;
    uint16_t raw_len = usb_gamepad_get_raw_report(&raw);

    printf("\n========== SNAPSHOT ==========\n");

    // ① 生HIDレポート
    printf("(1) HID raw [%u]:", raw_len);
    for (uint16_t i = 0; i < raw_len && raw; i++) printf(" %02X", raw[i]);
    printf("\n");

    // ② デコード後 gamepad_state
    printf("(2) state: LX=%d LY=%d RX=%d RY=%d L2=%d R2=%d BTN=0x%04X conn=%d VID=%04X PID=%04X\n",
           gp->axes[GAMEPAD_AXIS_LX], gp->axes[GAMEPAD_AXIS_LY],
           gp->axes[GAMEPAD_AXIS_RX], gp->axes[GAMEPAD_AXIS_RY],
           gp->axes[GAMEPAD_AXIS_L2], gp->axes[GAMEPAD_AXIS_R2],
           gp->buttons, gp->connected, gp->vid, gp->pid);

    // ③ CRSFチャンネル（normalize後）
    printf("(3) CRSF ch:");
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) printf(" %d", crsf_channels[i]);
    printf("\n");

    // ④ パック後payload（22バイト）
    printf("(4) payload[22]:");
    for (int i = 0; i < CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE; i++) printf(" %02X", frame[3 + i]);
    printf("\n");

    // ⑤ フレーム全体 + CRC検証
    printf("(5) frame[%u]:", (unsigned)frame_len);
    for (size_t i = 0; i < frame_len; i++) printf(" %02X", frame[i]);
    uint8_t crc_calc = crsf_crc8(&frame[2], CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE + 1);
    uint8_t crc_pkt = frame[frame_len - 1];
    printf("\n    SYNC=%02X LEN=%u TYPE=%02X CRC=%02X (calc=%02X %s)\n",
           frame[0], frame[1], frame[2], crc_pkt, crc_calc,
           (crc_pkt == crc_calc) ? "OK" : "NG");

    // ⑥ ドレインしたテレメトリ
    printf("(6) drained telemetry [%u]:", dbg_drain_len);
    for (uint16_t i = 0; i < dbg_drain_len; i++) printf(" %02X", dbg_drain_buf[i]);
    printf("\n==============================\n");
}

// (C) 連続ストリーム: PCビューア向けの1行CSV（プレフィックス "D,"）
//   D,conn,a0..a5,btn,ch0..ch15
static void dbg_stream_frame(void) {
    if (!dbg_stream_enabled) return;
    if (++dbg_stream_counter < DEBUG_STREAM_DECIMATION) return;
    dbg_stream_counter = 0;

    const gamepad_state_t *gp = usb_gamepad_get_state();
    printf("D,%d", gp->connected ? 1 : 0);
    for (int i = 0; i < 6; i++) printf(",%d", gp->axes[i]);
    printf(",%u", gp->buttons);
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) printf(",%d", crsf_channels[i]);
    printf("\n");
}

// デバッグUART RX(GP5)からのコマンドを処理（非ブロッキング）
static void dbg_poll_commands(void) {
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        switch (c) {
            case 'd': case 'D':
                dbg_snapshot_request = true;
                break;
            case 's': case 'S':
                dbg_stream_enabled = !dbg_stream_enabled;
                printf("# stream %s\n", dbg_stream_enabled ? "ON" : "OFF");
                break;
            case 'p': case 'P':
                playback_toggle();  // PLAYBACKモードでのみ有効
                break;
            case 'i': case 'I':
                input_profile_toggle();
                break;
            case '?':
                printf("# commands: d=snapshot  s=toggle stream  p=play/stop(PLAYBACK)"
                       "  i=input profile  ?=help\n");
                printf("# mode button(GP%d): short=cycle mode  long(%.1fs)=play/stop in PLAYBACK\n",
                       MODE_BTN_PIN, MODE_BTN_LONG_MS / 1000.0);
                printf("# input button(GP%d): switch GENERIC(accum) <-> LITERADIO(direct)"
                       " [now: %s]\n", INPUT_BTN_PIN, input_name(current_input));
                break;
            default:
                break;
        }
    }
}

// ========================================
// 初期化
// ========================================

static void init_crsf_uart(void) {
    // CRSF用UART初期化
    uart_init(CRSF_UART, CRSF_BAUD_RATE);
    gpio_set_function(CRSF_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(CRSF_UART_RX, GPIO_FUNC_UART);

    // 8N1設定
    uart_set_format(CRSF_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(CRSF_UART, true);

#if CRSF_INVERT_SIGNAL
    // Nano TX V2 の S.Port は反転UART。TX出力とRX入力を反転する。
    gpio_set_outover(CRSF_UART_TX, GPIO_OVERRIDE_INVERT);
    gpio_set_inover(CRSF_UART_RX, GPIO_OVERRIDE_INVERT);
    printf("CRSF UART signal inversion: ON\n");
#endif

    printf("CRSF UART initialized: %d bps on GP%d\n", CRSF_BAUD_RATE, CRSF_UART_TX);
}

// デバッグUART(uart1/GP4-5) は stdio_init_all() が CMake の
// PICO_DEFAULT_UART=1 設定に従って初期化するため、ここでの個別初期化は不要。

static void init_led(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
}

// ========================================
// モード切替 (ボタン + LED)
// ========================================

static const char* mode_name(op_mode_t m) {
    switch (m) {
        case MODE_SIMPLE:   return "SIMPLE";
        case MODE_RECORD:   return "RECORD";
        case MODE_PLAYBACK: return "PLAYBACK";
        default:            return "?";
    }
}

// 現在のモードに応じて3つの外付けLEDを1つだけ点灯させる。
static void update_mode_leds(void) {
    gpio_put(LED_RECORD_PIN,   current_mode == MODE_RECORD);
    gpio_put(LED_PLAYBACK_PIN, current_mode == MODE_PLAYBACK);
    gpio_put(LED_SIMPLE_PIN,   current_mode == MODE_SIMPLE);
}

static const char* input_name(input_profile_t p) {
    switch (p) {
        case INPUT_GENERIC:   return "GENERIC(accum)";
        case INPUT_LITERADIO: return "LITERADIO(direct)";
        default:              return "?";
    }
}

// 現在の入力プロファイルに応じて2つのLEDを1つだけ点灯させる。
static void update_input_leds(void) {
    gpio_put(LED_INPUT_GENERIC_PIN,   current_input == INPUT_GENERIC);
    gpio_put(LED_INPUT_LITERADIO_PIN, current_input == INPUT_LITERADIO);
}

static void init_mode_io(void) {
    // ボタン: 入力 + 内部プルアップ（押下でLowに落ちる）
    gpio_init(MODE_BTN_PIN);
    gpio_set_dir(MODE_BTN_PIN, GPIO_IN);
    gpio_pull_up(MODE_BTN_PIN);

    // 入力プロファイル切替ボタン: 同上
    gpio_init(INPUT_BTN_PIN);
    gpio_set_dir(INPUT_BTN_PIN, GPIO_IN);
    gpio_pull_up(INPUT_BTN_PIN);

    // モードLED: 出力
    gpio_init(LED_RECORD_PIN);   gpio_set_dir(LED_RECORD_PIN,   GPIO_OUT);
    gpio_init(LED_PLAYBACK_PIN); gpio_set_dir(LED_PLAYBACK_PIN, GPIO_OUT);
    gpio_init(LED_SIMPLE_PIN);   gpio_set_dir(LED_SIMPLE_PIN,   GPIO_OUT);

    // 入力プロファイルLED: 出力
    gpio_init(LED_INPUT_GENERIC_PIN);
    gpio_set_dir(LED_INPUT_GENERIC_PIN, GPIO_OUT);
    gpio_init(LED_INPUT_LITERADIO_PIN);
    gpio_set_dir(LED_INPUT_LITERADIO_PIN, GPIO_OUT);

    update_mode_leds();
    update_input_leds();
}

// 入力プロファイルを切り替える。スロットルの解釈が変わるため、
// 積算値は必ず最小へリセットしてから切り替える。
static void input_profile_toggle(void) {
    current_input = (input_profile_t)((current_input + 1) % INPUT_PROFILE_COUNT);
    throttle_reset();
    update_input_leds();
    printf("# input -> %s\n", input_name(current_input));
}

// 入力プロファイル切替ボタン（GP6）をポーリング。短押しでトグル。
static void poll_input_button(void) {
    bool raw = (gpio_get(INPUT_BTN_PIN) == 0);  // 押下=true
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (raw != ibtn_pressed_state && (now - ibtn_last_change_ms) >= INPUT_BTN_DEBOUNCE_MS) {
        ibtn_last_change_ms = now;
        ibtn_pressed_state = raw;
        if (!raw) {  // 離したときに確定（押しっぱなしでの連続切替を防ぐ）
            input_profile_toggle();
        }
    }
}

// モードを1つ進め、記録の開始/停止など遷移に伴う処理を行う（短押し時）。
static void mode_advance(void) {
    op_mode_t prev = current_mode;
    current_mode = (op_mode_t)((current_mode + 1) % MODE_COUNT);
    update_mode_leds();
    printf("# mode -> %s\n", mode_name(current_mode));

    // RECORDから抜けたら記録停止＋フラッシュ書出し
    if (prev == MODE_RECORD && current_mode != MODE_RECORD) {
        size_t n = recorder_stop_and_flush();
        printf("# recording stopped, flushed %u samples to flash\n", (unsigned)n);
    }
    // RECORDに入ったら記録開始（バッファクリア）
    if (current_mode == MODE_RECORD && prev != MODE_RECORD) {
        recorder_start();
        printf("# recording started\n");
    }
    // PLAYBACKに入ったら再生を待機状態にし、保存済みサンプル数を表示
    if (current_mode == MODE_PLAYBACK) {
        playback_running = false;
        pb_sample_idx = 0;
        pb_frame_ctr = 0;
        printf("# playback ready: %u samples in flash (long-press or 'p' to play)\n",
               (unsigned)recorder_flash_sample_count());
    }
}

// ボタンをポーリング（非ブロッキング）。
//   短押し（長押し未満で離す）= モードを1つ進める
//   長押し（MODE_BTN_LONG_MS 保持）= PLAYBACK中なら再生start/stop
// 長押し中はモード循環を抑止する。
static void poll_mode_button(void) {
    bool raw = (gpio_get(MODE_BTN_PIN) == 0);  // 押下=true
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // デバウンス後の状態変化を確定
    if (raw != btn_pressed_state && (now - btn_last_change_ms) >= MODE_BTN_DEBOUNCE_MS) {
        btn_last_change_ms = now;
        btn_pressed_state = raw;
        if (raw) {
            // 押下開始: まだ何もしない（短/長を離すまで/しきい値で判定）
            btn_press_start_ms = now;
            btn_long_fired = false;
        } else {
            // 離した: 長押しが発火していなければ短押し = モード循環
            if (!btn_long_fired) {
                mode_advance();
            }
        }
    }

    // 保持中に長押ししきい値を超えたら一度だけ長押しアクション
    if (btn_pressed_state && !btn_long_fired &&
        (now - btn_press_start_ms) >= MODE_BTN_LONG_MS) {
        btn_long_fired = true;  // 短押し扱いを抑止
        if (current_mode == MODE_PLAYBACK) {
            playback_toggle();
        }
    }
}

static void init_channels(void) {
    // 全チャンネルを安全な初期値に設定
    // スロットルは最小、他は中央
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        crsf_channels[i] = CRSF_CHANNEL_MID;
    }
    // スロットル（CH3）は最小値
    crsf_channels[2] = CRSF_CHANNEL_MIN;
}

// ========================================
// メインループ
// ========================================

int main() {
    // stdio初期化（デバッグUARTへ出力）
    stdio_init_all();

    printf("\n");
    printf("========================================\n");
    printf("  ExpressLRS Realtime Recorder\n");
    printf("========================================\n");
    printf("\n");

    // 初期化
    init_led();
    init_mode_io();
    init_channels();
    init_crsf_uart();
    recorder_init();
    usb_gamepad_init();

    printf("Mode: %s (press GP%d button to cycle)\n", mode_name(current_mode), MODE_BTN_PIN);
    printf("Input: %s (press GP%d button to switch)\n", input_name(current_input), INPUT_BTN_PIN);

    printf("USB Host initialized, waiting for gamepad...\n");
    printf("\n");

    // CRSFパケットバッファ
    uint8_t crsf_packet[CRSF_MAX_PACKET_SIZE];

    while (1) {
        // TinyUSB Hostタスク処理
        usb_gamepad_task();

        // デバッグUARTからのコマンド処理（d=snapshot, s=stream, ?=help）
        dbg_poll_commands();

        // モード切替ボタン（GP2）・入力プロファイル切替ボタン（GP6）のポーリング
        poll_mode_button();
        poll_input_button();

        // 現在時刻を取得
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // CRSF送信間隔チェック
        if (now - last_crsf_send_time >= CRSF_SEND_INTERVAL_MS) {
            last_crsf_send_time = now;

            // ゲームパッド状態を取得
            const gamepad_state_t *gamepad = usb_gamepad_get_state();

            // 再生中に送出するフラッシュサンプル（無ければNULL）
            const uint8_t *pb_payload = NULL;

            switch (current_mode) {
                case MODE_SIMPLE:
                case MODE_RECORD:
                    // 単純/記録: ゲームパッド入力を channels にマッピング
                    if (gamepad->connected) {
                        map_gamepad_to_channels(gamepad, now);
                        gpio_put(LED_PIN, 1);  // 接続中: 内蔵LED点灯
                    } else {
                        // 未接続: フェイルセーフ値を送出（スロットル最小・Arm解除）。
                        // 最後の値を送り続けると機体がリンク正常と誤認するため。
                        set_failsafe_channels();
                        gpio_put(LED_PIN, (now / 500) % 2);  // 点滅
                    }
                    // 記録: 現在の16chを記録器へ（内部で50Hz間引き＋ボタンラッチ）
                    if (current_mode == MODE_RECORD) {
                        recorder_on_frame(crsf_channels);
                        // バッファ満杯時は記録LEDを速い点滅で警告
                        if (recorder_is_full()) {
                            gpio_put(LED_RECORD_PIN, (now / 150) % 2);
                        }
                    }
                    break;

                case MODE_PLAYBACK:
                    if (playback_running) {
                        pb_payload = recorder_flash_sample(pb_sample_idx);
                    }
                    if (pb_payload == NULL) {
                        // 停止中/データ無し: 安全側（スロットル最小・他中央）を送出
                        for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
                            crsf_channels[i] = CRSF_CHANNEL_MID;
                        }
                        crsf_channels[2] = CRSF_CHANNEL_MIN;
                    }
                    gpio_put(LED_PIN, (now / 250) % 2);   // 速い点滅=再生モード
                    break;

                default:
                    break;
            }

            // CRSFパケットを生成して送信
            //   再生中は記録済みpayloadをそのままフレーム化、それ以外はchannelsから生成
            size_t len = pb_payload
                ? crsf_build_frame_from_payload(pb_payload, crsf_packet)
                : crsf_build_rc_channels_packet(crsf_channels, crsf_packet);
            crsf_uart_send(crsf_packet, len);

            // 半二重バス上のテレメトリ応答を読み捨てて衝突を防ぐ
            crsf_drain_telemetry();

            // 再生の50Hz送出: REC_DECIMATION フレームごとに次サンプルへ
            if (pb_payload) {
                if (++pb_frame_ctr >= REC_DECIMATION) {
                    pb_frame_ctr = 0;
                    if (++pb_sample_idx >= recorder_flash_sample_count()) {
                        pb_sample_idx = 0;
                        playback_running = false;  // 末尾まで再生したら停止
                        printf("# playback finished\n");
                    }
                }
            }

            // (C) 連続ストリーム（有効時のみ、間引き出力）
            dbg_stream_frame();

            // (A) スナップショット要求があれば全段ダンプ
            if (dbg_snapshot_request) {
                dbg_snapshot_request = false;
                dbg_dump_snapshot(crsf_packet, len);
            }

            // デバッグログ（1秒ごとのヒートビート）
            debug_log_channels();
        }
    }

    return 0;
}

#else
// ========================================
// ホストPC用スタブ（テスト用）
// ========================================

int main() {
    printf("ExpressLRS Realtime Recorder\n");
    printf("This is a Pico application. Build with Pico SDK.\n");
    printf("\n");
    printf("To build for Pico:\n");
    printf("  export PICO_SDK_PATH=/path/to/pico-sdk\n");
    printf("  mkdir build && cd build\n");
    printf("  cmake ..\n");
    printf("  make\n");
    return 0;
}

#endif // PICO_BOARD
