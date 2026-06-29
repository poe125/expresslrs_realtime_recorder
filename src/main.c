#include <stdio.h>
#include <string.h>
#include "crsf.h"
#include "usb_gamepad.h"

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

// LED（状態表示用）
#define LED_PIN         PICO_DEFAULT_LED_PIN

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

// ゲームパッド入力をCRSFチャンネルにマッピング
static void map_gamepad_to_channels(const gamepad_state_t *gamepad) {
    // 標準的なドローン操縦マッピング (Mode 2)
    // CH1: Roll     (右スティック X)
    // CH2: Pitch    (右スティック Y) ※反転
    // CH3: Throttle (左スティック Y) ※反転
    // CH4: Yaw      (左スティック X)

    crsf_channels[0] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_RX]);  // Roll
    crsf_channels[1] = crsf_normalize_channel(-gamepad->axes[GAMEPAD_AXIS_RY]); // Pitch (反転)
    crsf_channels[2] = crsf_normalize_channel(-gamepad->axes[GAMEPAD_AXIS_LY]); // Throttle (反転)
    crsf_channels[3] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_LX]);  // Yaw

    // CH5-8: トリガーとボタン
    // L2トリガー → CH5
    crsf_channels[4] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_L2]);
    // R2トリガー → CH6
    crsf_channels[5] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_R2]);

    // ボタンをスイッチチャンネルにマッピング
    // A/Crossボタン → CH7 (Arm)
    crsf_channels[6] = (gamepad->buttons & GAMEPAD_BTN_A) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
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
            case '?':
                printf("# commands: d=snapshot  s=toggle stream  ?=help\n");
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
    init_channels();
    init_crsf_uart();
    usb_gamepad_init();

    printf("USB Host initialized, waiting for gamepad...\n");
    printf("\n");

    // CRSFパケットバッファ
    uint8_t crsf_packet[CRSF_MAX_PACKET_SIZE];

    while (1) {
        // TinyUSB Hostタスク処理
        usb_gamepad_task();

        // デバッグUARTからのコマンド処理（d=snapshot, s=stream, ?=help）
        dbg_poll_commands();

        // 現在時刻を取得
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // CRSF送信間隔チェック
        if (now - last_crsf_send_time >= CRSF_SEND_INTERVAL_MS) {
            last_crsf_send_time = now;

            // ゲームパッド状態を取得してチャンネルにマッピング
            const gamepad_state_t *gamepad = usb_gamepad_get_state();

            if (gamepad->connected) {
                // 接続中: ゲームパッド入力をマッピング
                map_gamepad_to_channels(gamepad);
                gpio_put(LED_PIN, 1);  // LED ON
            } else {
                // 未接続: 安全な値を維持（スロットル最小）
                // チャンネル値は変更しない（最後の値または初期値を維持）
                gpio_put(LED_PIN, (now / 500) % 2);  // LED点滅
            }

            // CRSFパケットを生成して送信
            size_t len = crsf_build_rc_channels_packet(crsf_channels, crsf_packet);
            crsf_uart_send(crsf_packet, len);

            // 半二重バス上のテレメトリ応答を読み捨てて衝突を防ぐ
            crsf_drain_telemetry();

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
