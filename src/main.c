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
#define CRSF_UART_RX    1   // GP1 (受信は使用しないが設定は必要)
#define CRSF_BAUD_RATE  420000

// デバッグ用UART
#define DEBUG_UART      uart1
#define DEBUG_UART_TX   4   // GP4
#define DEBUG_UART_RX   5   // GP5
#define DEBUG_BAUD_RATE 115200

// LED（状態表示用）
#define LED_PIN         PICO_DEFAULT_LED_PIN

// ========================================
// CRSF送信設定
// ========================================

// CRSF送信間隔（ミリ秒）
// ExpressLRS 500Hz = 2ms間隔
#define CRSF_SEND_INTERVAL_MS  4  // 250Hz（安定性重視）

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

// ========================================
// デバッグログ
// ========================================

static void debug_log_channels(void) {
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

    printf("CRSF UART initialized: %d bps on GP%d\n", CRSF_BAUD_RATE, CRSF_UART_TX);
}

static void init_debug_uart(void) {
    // デバッグ用UART初期化
    uart_init(DEBUG_UART, DEBUG_BAUD_RATE);
    gpio_set_function(DEBUG_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(DEBUG_UART_RX, GPIO_FUNC_UART);

    // stdoutをUART1にリダイレクト
    // (stdio_uart_init_fullでも可)
}

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

            // デバッグログ
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
