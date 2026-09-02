#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "crsf.h"
#include "usb_gamepad.h"
#include "recorder.h"

#if defined(PICO_BOARD)
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "tusb.h"
#elif defined(BUILD_PI4)
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#endif

#if defined(PICO_BOARD) || defined(BUILD_PI4)

// ========================================
// ピン/デバイス設定
// ========================================

#if defined(PICO_BOARD)
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

// LED（内蔵: 接続/再生ステータス表示用）
#define LED_PIN         PICO_DEFAULT_LED_PIN

// ========================================
// モード切替 (ボタン + 外付けLED)
// ========================================
// 参照図(63bit氏 R63b)の空きGPIO配置に合わせる。
// ボタン: GP2 にタクトSW（内部プルアップ, 押下=Low）。押すたびにモード循環。
#define MODE_BTN_PIN     2
// モード表示LED（各 100Ω 直列で GND へ）
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
#define INPUT_BTN_PIN            6
#define LED_INPUT_GENERIC_PIN    13  // 汎用ゲームパッド（スロットル積算）
#define LED_INPUT_LITERADIO_PIN  14  // LiteRadio3（スロットル絶対値）
#define INPUT_BTN_DEBOUNCE_MS    30

#elif defined(BUILD_PI4)
// Raspberry Pi 4: CRSF出力はUART5（GPIO12=TX/GPIO13=RX, config.txtの
// `dtoverlay=uart5` で有効化）を使う。BCM2711のPL011はRP2040のGPIOオーバー
// ライドのようなハードウェア信号反転を持たないため、Nano TX Module V2の
// 反転S.Port信号に合わせるには外付けの信号反転回路が別途必要
// （詳細は CLAUDE.md の「Raspberry Pi 4版: CRSF信号反転」を参照）。
#define CRSF_UART_PI4_PATH  "/dev/ttyAMA5"
#define CRSF_BAUD_RATE      921600
#endif

// ========================================
// CRSF送信設定
// ========================================

// CRSF送信間隔（ミリ秒）
// ExpressLRS 500Hz = 2ms間隔
#define CRSF_SEND_INTERVAL_MS  2  // 500Hz（senderと同じ。ELRS V3.x標準）

// スロットル積算のパラメータ（INPUT_GENERIC時のみ有効）
#define THROTTLE_FULL_TRAVEL_MS  2000
#define THROTTLE_DEADZONE        3000
#define THROTTLE_MAX_DT_MS       20

// ========================================
// グローバル変数
// ========================================

// CRSFチャンネル値
static uint16_t crsf_channels[CRSF_NUM_CHANNELS];

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
// 連続ストリーム(C)の間引き: 500Hz / 20 = 25Hz で出力
#define DEBUG_STREAM_DECIMATION 20
// 直近のドレインで読み捨てたテレメトリ（スナップショット(6)用）
static uint8_t dbg_drain_buf[32];
static uint16_t dbg_drain_len = 0;

// ========================================
// 入力プロファイル（ボタン押下で GENERIC ⇔ LITERADIO をトグル）
// ========================================
typedef enum {
    INPUT_GENERIC = 0,   // 汎用ゲームパッド: 左スティック上下=スロットルの増減率
    INPUT_LITERADIO,     // LiteRadio3: 左スティック位置=スロットル絶対値
    INPUT_PROFILE_COUNT
} input_profile_t;

// 起動時は手持ちの汎用ゲームパッド。LiteRadioは接続時に手動で切り替える。
static input_profile_t current_input = INPUT_GENERIC;

// スロットル積算値（CRSF単位×1000のミリ単位で保持し、丸め誤差の蓄積を防ぐ）
static int32_t  throttle_accum_milli = (int32_t)CRSF_CHANNEL_MIN * 1000;
static uint32_t throttle_last_update_ms = 0;

// 実体は後述。UARTコマンド 'i' からも呼ぶため先に宣言する。
static void input_profile_toggle(void);
static const char* input_name(input_profile_t p);
static void throttle_reset(void);

// 動作モード（SIMPLE→RECORD→PLAYBACK→… と循環）
typedef enum {
    MODE_SIMPLE = 0,   // 単純: gamepad → CRSF（パススルー）
    MODE_RECORD,       // 記録: gamepad → CRSF ＋ ログ保存
    MODE_PLAYBACK,     // 再生: 保存済みログ → CRSF
    MODE_COUNT
} op_mode_t;

// 起動時は最も安全な単純パススルー。誤って再生で機体を駆動しないため。
static op_mode_t current_mode = MODE_SIMPLE;

// 再生状態（PLAYBACKモードで長押し(Pico)/'p'コマンドにより開始/停止）
static bool     playback_running = false;
static uint32_t pb_sample_idx = 0;   // 次に送出するフラッシュサンプル番号
static uint16_t pb_frame_ctr = 0;    // 50Hz送出のための500Hzフレームカウンタ

static const char* mode_name(op_mode_t m) {
    switch (m) {
        case MODE_SIMPLE:   return "SIMPLE";
        case MODE_RECORD:   return "RECORD";
        case MODE_PLAYBACK: return "PLAYBACK";
        default:            return "?";
    }
}

static const char* input_name(input_profile_t p) {
    switch (p) {
        case INPUT_GENERIC:   return "GENERIC(accum)";
        case INPUT_LITERADIO: return "LITERADIO(direct)";
        default:              return "?";
    }
}

#ifdef PICO_BOARD
// ========================================
// モード/入力プロファイル用LED（Pico限定。Pi4版はボタン/LEDを持たない）
// ========================================

// ボタン状態（デバウンス＋短押し/長押し判別用）
static bool     btn_pressed_state = false;   // 確定済みの押下状態
static uint32_t btn_last_change_ms = 0;
static uint32_t btn_press_start_ms = 0;      // 押下開始時刻
static bool     btn_long_fired = false;      // 今回の押下で長押しアクション済みか

// 入力プロファイル切替ボタンの状態（デバウンス用）
static bool     ibtn_pressed_state = false;
static uint32_t ibtn_last_change_ms = 0;

// 現在のモードに応じて3つの外付けLEDを1つだけ点灯させる。
static void update_mode_leds(void) {
    gpio_put(LED_RECORD_PIN,   current_mode == MODE_RECORD);
    gpio_put(LED_PLAYBACK_PIN, current_mode == MODE_PLAYBACK);
    gpio_put(LED_SIMPLE_PIN,   current_mode == MODE_SIMPLE);
}

// 現在の入力プロファイルに応じて2つのLEDを1つだけ点灯させる。
static void update_input_leds(void) {
    gpio_put(LED_INPUT_GENERIC_PIN,   current_input == INPUT_GENERIC);
    gpio_put(LED_INPUT_LITERADIO_PIN, current_input == INPUT_LITERADIO);
}
#endif // PICO_BOARD

// 再生の開始/停止をトグル（PLAYBACKモードでのみ有効）。
// long-press(Pico) と 'p' コマンドの両方から使う共通処理。
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

// 入力プロファイルを切り替える。スロットルの解釈が変わるため、
// 積算値は必ず最小へリセットしてから切り替える。
static void input_profile_toggle(void) {
    current_input = (input_profile_t)((current_input + 1) % INPUT_PROFILE_COUNT);
    throttle_reset();
#ifdef PICO_BOARD
    update_input_leds();
#endif
    printf("# input -> %s\n", input_name(current_input));
}

// モードを1つ進め、記録の開始/停止など遷移に伴う処理を行う。
static void mode_advance(void) {
    op_mode_t prev = current_mode;
    current_mode = (op_mode_t)((current_mode + 1) % MODE_COUNT);
#ifdef PICO_BOARD
    update_mode_leds();
#endif
    printf("# mode -> %s\n", mode_name(current_mode));

    // RECORDから抜けたら記録停止＋書出し
    if (prev == MODE_RECORD && current_mode != MODE_RECORD) {
        size_t n = recorder_stop_and_flush();
        printf("# recording stopped, flushed %u samples\n", (unsigned)n);
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
        printf("# playback ready: %u samples stored (command 'p' to play)\n",
               (unsigned)recorder_flash_sample_count());
    }
}

// ========================================
// チャンネルマッピング（プラットフォーム非依存）
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
static void throttle_reset(void) {
    throttle_accum_milli = (int32_t)CRSF_CHANNEL_MIN * 1000;
}

// 汎用ゲームパッド用のスロットル積算。
static uint16_t throttle_accumulate(int16_t stick_up, uint32_t now) {
    uint32_t dt = now - throttle_last_update_ms;
    throttle_last_update_ms = now;
    if (dt > THROTTLE_MAX_DT_MS) dt = THROTTLE_MAX_DT_MS;

    int32_t mag = stick_up < 0 ? -(int32_t)stick_up : (int32_t)stick_up;
    if (mag > THROTTLE_DEADZONE) {
        mag = (mag - THROTTLE_DEADZONE) * 32767 / (32767 - THROTTLE_DEADZONE);
        int32_t range_milli = (int32_t)(CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN) * 1000;
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

// ラジオ送信機（LiteRadio 3 等）用のマッピング。素通し。
static void map_radio_to_channels(const gamepad_state_t *gamepad) {
    uint8_t n = gamepad->channel_count;
    if (n > 6) n = 6;
    for (uint8_t c = 0; c < n; c++) {
        crsf_channels[c] = crsf_normalize_channel(gamepad->channels[c]);
    }
    crsf_channels[6]  = (gamepad->buttons & GAMEPAD_BTN_A)  ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[7]  = (gamepad->buttons & GAMEPAD_BTN_B)  ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[8]  = (gamepad->buttons & GAMEPAD_BTN_X)  ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[9]  = (gamepad->buttons & GAMEPAD_BTN_Y)  ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[10] = (gamepad->buttons & GAMEPAD_BTN_LB) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[11] = (gamepad->buttons & GAMEPAD_BTN_RB) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    for (int i = 12; i < CRSF_NUM_CHANNELS; i++) {
        crsf_channels[i] = CRSF_CHANNEL_MID;
    }
}

// ゲームパッド入力をCRSFチャンネルにマッピング
static void map_gamepad_to_channels(const gamepad_state_t *gamepad, uint32_t now) {
    crsf_channels[0] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_RX]);  // Roll
    crsf_channels[1] = crsf_normalize_channel(axis_invert(gamepad->axes[GAMEPAD_AXIS_RY])); // Pitch (反転)
    crsf_channels[3] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_LX]);  // Yaw

    int16_t stick_up = axis_invert(gamepad->axes[GAMEPAD_AXIS_LY]);  // 上=正
    if (current_input == INPUT_GENERIC) {
        crsf_channels[2] = throttle_accumulate(stick_up, now);
    } else {
        crsf_channels[2] = crsf_normalize_channel(stick_up);
    }

    crsf_channels[4] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_L2]);
    crsf_channels[5] = crsf_normalize_channel(gamepad->axes[GAMEPAD_AXIS_R2]);

    if ((gamepad->buttons & GAMEPAD_BTN_A) && !(prev_buttons & GAMEPAD_BTN_A)) {
        arm_latched = !arm_latched;
        if (!arm_latched) throttle_reset();
        printf("# %s\n", arm_latched ? "ARM" : "DISARM");
    }
    prev_buttons = gamepad->buttons;
    crsf_channels[6] = arm_latched ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[7] = (gamepad->buttons & GAMEPAD_BTN_B) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;

    crsf_channels[8]  = (gamepad->buttons & GAMEPAD_BTN_X) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[9]  = (gamepad->buttons & GAMEPAD_BTN_Y) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[10] = (gamepad->buttons & GAMEPAD_BTN_LB) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;
    crsf_channels[11] = (gamepad->buttons & GAMEPAD_BTN_RB) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN;

    crsf_channels[12] = CRSF_CHANNEL_MID;
    crsf_channels[13] = CRSF_CHANNEL_MID;
    crsf_channels[14] = CRSF_CHANNEL_MID;
    crsf_channels[15] = CRSF_CHANNEL_MID;
}

// 入力喪失時（ゲームパッド切断）に送るフェイルセーフ値。
static void set_failsafe_channels(void) {
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        crsf_channels[i] = CRSF_CHANNEL_MID;
    }
    crsf_channels[2] = CRSF_CHANNEL_MIN;  // CH3 Throttle 最小
    crsf_channels[6] = CRSF_CHANNEL_MIN;  // CH7 Arm 解除
    arm_latched = false;
    prev_buttons = 0;
    throttle_reset();
}

// ========================================
// UART送信（プラットフォーム別実装）
// ========================================

#if defined(PICO_BOARD)

static void crsf_uart_send(const uint8_t *data, size_t len) {
    uart_write_blocking(CRSF_UART, data, len);
}

// 半二重テレメトリドレイン
static void crsf_drain_telemetry(void) {
    uart_tx_wait_blocking(CRSF_UART);

    dbg_drain_len = 0;
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

static void init_crsf_uart(void) {
    uart_init(CRSF_UART, CRSF_BAUD_RATE);
    gpio_set_function(CRSF_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(CRSF_UART_RX, GPIO_FUNC_UART);

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

#elif defined(BUILD_PI4)

static int crsf_fd = -1;

static void init_crsf_uart(void) {
    crsf_fd = open(CRSF_UART_PI4_PATH, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (crsf_fd < 0) {
        perror("# CRSF UART: open failed");
        printf("# continuing WITHOUT CRSF output (check wiring/permissions on %s)\n",
               CRSF_UART_PI4_PATH);
        return;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(crsf_fd, &tio) != 0) {
        perror("# CRSF UART: tcgetattr failed");
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, B921600);
    cfsetospeed(&tio, B921600);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(crsf_fd, TCSANOW, &tio) != 0) {
        perror("# CRSF UART: tcsetattr failed");
    }

    printf("CRSF UART initialized: %d bps on %s\n", CRSF_BAUD_RATE, CRSF_UART_PI4_PATH);
    printf("# NOTE: BCM2711 UART has no hardware signal inversion.\n");
    printf("#       Nano TX Module V2 needs an external inverter circuit\n");
    printf("#       between GPIO12/13 and the module (see CLAUDE.md).\n");
}

static void crsf_uart_send(const uint8_t *data, size_t len) {
    if (crsf_fd < 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(crsf_fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)n;
    }
}

// 半二重テレメトリドレイン（Picoのuart_is_readable/busy_wait_us相当）。
// バス上のバイトを最大300us・無通信60us検出まで読み捨てる。
//
// 注意: tcdrain()（TCSBRK ioctl）は本機のPL011ドライバでは1回あたり約8msも
// かかり（ジフィー単位のポーリング実装らしく、Picoのuart_tx_wait_blocking
// のようなレジスタ直読みの即時ポーリングとは全く違う）、500Hz送信の予算
// 2ms を大きく超えてループを詰まらせるため、意図的に呼ばない。
// 26バイト@921600bpsの送信自体は約280usで終わるため、後続の
// 最大300usアイドル検出ループの中で自然に完了を待てる。
static void crsf_drain_telemetry(void) {
    dbg_drain_len = 0;
    if (crsf_fd < 0) return;

    struct timespec start, last_byte, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    last_byte = start;

    for (;;) {
        uint8_t buf[32];
        ssize_t n = read(crsf_fd, buf, sizeof(buf));
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (n > 0) {
            for (ssize_t i = 0; i < n && dbg_drain_len < sizeof(dbg_drain_buf); i++) {
                dbg_drain_buf[dbg_drain_len++] = buf[i];
            }
            last_byte = now;
        }
        long since_start_us = (now.tv_sec - start.tv_sec) * 1000000L
                             + (now.tv_nsec - start.tv_nsec) / 1000L;
        long since_byte_us  = (now.tv_sec - last_byte.tv_sec) * 1000000L
                             + (now.tv_nsec - last_byte.tv_nsec) / 1000L;
        if (since_byte_us >= 60 || since_start_us >= 300) break;
    }
}

#endif

// ========================================
// デバッグログ
// ========================================

static void debug_log_channels(void) {
    if (dbg_stream_enabled) return;

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

static void dbg_dump_snapshot(const uint8_t *frame, size_t frame_len) {
    const gamepad_state_t *gp = usb_gamepad_get_state();
    const uint8_t *raw = NULL;
    uint16_t raw_len = usb_gamepad_get_raw_report(&raw);

    printf("\n========== SNAPSHOT ==========\n");

    printf("(1) HID raw [%u]:", raw_len);
    for (uint16_t i = 0; i < raw_len && raw; i++) printf(" %02X", raw[i]);
    printf("\n");

    printf("(2) state: LX=%d LY=%d RX=%d RY=%d L2=%d R2=%d BTN=0x%04X conn=%d VID=%04X PID=%04X\n",
           gp->axes[GAMEPAD_AXIS_LX], gp->axes[GAMEPAD_AXIS_LY],
           gp->axes[GAMEPAD_AXIS_RX], gp->axes[GAMEPAD_AXIS_RY],
           gp->axes[GAMEPAD_AXIS_L2], gp->axes[GAMEPAD_AXIS_R2],
           gp->buttons, gp->connected, gp->vid, gp->pid);

    if (gp->channel_count > 0) {
        printf("(2') channels:");
        for (uint8_t c = 0; c < gp->channel_count; c++) {
            printf(" c%u=%d", c + 1, gp->channels[c]);
        }
        printf("\n");
    }

    printf("(3) CRSF ch:");
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) printf(" %d", crsf_channels[i]);
    printf("\n");

    printf("(4) payload[22]:");
    for (int i = 0; i < CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE; i++) printf(" %02X", frame[3 + i]);
    printf("\n");

    printf("(5) frame[%u]:", (unsigned)frame_len);
    for (size_t i = 0; i < frame_len; i++) printf(" %02X", frame[i]);
    uint8_t crc_calc = crsf_crc8(&frame[2], CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE + 1);
    uint8_t crc_pkt = frame[frame_len - 1];
    printf("\n    SYNC=%02X LEN=%u TYPE=%02X CRC=%02X (calc=%02X %s)\n",
           frame[0], frame[1], frame[2], crc_pkt, crc_calc,
           (crc_pkt == crc_calc) ? "OK" : "NG");

    printf("(6) drained telemetry [%u]:", dbg_drain_len);
    for (uint16_t i = 0; i < dbg_drain_len; i++) printf(" %02X", dbg_drain_buf[i]);
    printf("\n==============================\n");
}

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

// コマンド入力の非ブロッキング1文字読み取り（プラットフォーム別）
#if defined(PICO_BOARD)
static inline int platform_getchar_nonblock(void) {
    int c = getchar_timeout_us(0);
    return (c == PICO_ERROR_TIMEOUT) ? -1 : c;
}
#elif defined(BUILD_PI4)
static inline int platform_getchar_nonblock(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? (int)c : -1;
}
#endif

// コマンド処理（非ブロッキング）。Pico=デバッグUART(GP5)、Pi4=標準入力。
static void dbg_poll_commands(void) {
    int c;
    while ((c = platform_getchar_nonblock()) != -1) {
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
            case 'm': case 'M':
                mode_advance();  // モードを1つ進める（Pi4にはモード切替ボタンが無いため）
                break;
            case '?':
                printf("# commands: d=snapshot  s=toggle stream  p=play/stop(PLAYBACK)"
                       "  i=input profile  m=cycle mode  ?=help\n");
#ifdef PICO_BOARD
                printf("# mode button(GP%d): short=cycle mode  long(%.1fs)=play/stop in PLAYBACK\n",
                       MODE_BTN_PIN, MODE_BTN_LONG_MS / 1000.0);
                printf("# input button(GP%d): switch GENERIC(accum) <-> LITERADIO(direct)"
                       " [now: %s]\n", INPUT_BTN_PIN, input_name(current_input));
#else
                printf("# now: mode=%s  input=%s\n", mode_name(current_mode), input_name(current_input));
#endif
                break;
            default:
                break;
        }
    }
}

static void init_channels(void) {
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        crsf_channels[i] = CRSF_CHANNEL_MID;
    }
    crsf_channels[2] = CRSF_CHANNEL_MIN;
}

#ifdef PICO_BOARD
// ========================================
// 初期化 / ボタンポーリング（Pico限定）
// ========================================

static void init_led(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
}

static void init_mode_io(void) {
    gpio_init(MODE_BTN_PIN);
    gpio_set_dir(MODE_BTN_PIN, GPIO_IN);
    gpio_pull_up(MODE_BTN_PIN);

    gpio_init(INPUT_BTN_PIN);
    gpio_set_dir(INPUT_BTN_PIN, GPIO_IN);
    gpio_pull_up(INPUT_BTN_PIN);

    gpio_init(LED_RECORD_PIN);   gpio_set_dir(LED_RECORD_PIN,   GPIO_OUT);
    gpio_init(LED_PLAYBACK_PIN); gpio_set_dir(LED_PLAYBACK_PIN, GPIO_OUT);
    gpio_init(LED_SIMPLE_PIN);   gpio_set_dir(LED_SIMPLE_PIN,   GPIO_OUT);

    gpio_init(LED_INPUT_GENERIC_PIN);
    gpio_set_dir(LED_INPUT_GENERIC_PIN, GPIO_OUT);
    gpio_init(LED_INPUT_LITERADIO_PIN);
    gpio_set_dir(LED_INPUT_LITERADIO_PIN, GPIO_OUT);

    update_mode_leds();
    update_input_leds();
}

static void poll_input_button(void) {
    bool raw = (gpio_get(INPUT_BTN_PIN) == 0);
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (raw != ibtn_pressed_state && (now - ibtn_last_change_ms) >= INPUT_BTN_DEBOUNCE_MS) {
        ibtn_last_change_ms = now;
        ibtn_pressed_state = raw;
        if (!raw) {
            input_profile_toggle();
        }
    }
}

static void poll_mode_button(void) {
    bool raw = (gpio_get(MODE_BTN_PIN) == 0);
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (raw != btn_pressed_state && (now - btn_last_change_ms) >= MODE_BTN_DEBOUNCE_MS) {
        btn_last_change_ms = now;
        btn_pressed_state = raw;
        if (raw) {
            btn_press_start_ms = now;
            btn_long_fired = false;
        } else {
            if (!btn_long_fired) {
                mode_advance();
            }
        }
    }

    if (btn_pressed_state && !btn_long_fired &&
        (now - btn_press_start_ms) >= MODE_BTN_LONG_MS) {
        btn_long_fired = true;
        if (current_mode == MODE_PLAYBACK) {
            playback_toggle();
        }
    }
}
#endif // PICO_BOARD

// ========================================
// メインループ
// ========================================

#if defined(PICO_BOARD)

int main() {
    stdio_init_all();

    printf("\n");
    printf("========================================\n");
    printf("  ExpressLRS Realtime Recorder\n");
    printf("========================================\n");
    printf("\n");

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

    uint8_t crsf_packet[CRSF_MAX_PACKET_SIZE];
    uint32_t last_crsf_send_time = 0;

    while (1) {
        usb_gamepad_task();
        dbg_poll_commands();
        poll_mode_button();
        poll_input_button();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (now - last_crsf_send_time >= CRSF_SEND_INTERVAL_MS) {
            last_crsf_send_time = now;

            const gamepad_state_t *gamepad = usb_gamepad_get_state();
            const uint8_t *pb_payload = NULL;

            switch (current_mode) {
                case MODE_SIMPLE:
                case MODE_RECORD:
                    if (gamepad->connected) {
                        if (current_input == INPUT_LITERADIO &&
                            gamepad->channel_count >= 4) {
                            map_radio_to_channels(gamepad);
                        } else {
                            map_gamepad_to_channels(gamepad, now);
                        }
                        gpio_put(LED_PIN, 1);
                    } else {
                        set_failsafe_channels();
                        gpio_put(LED_PIN, (now / 500) % 2);
                    }
                    if (current_mode == MODE_RECORD) {
                        recorder_on_frame(crsf_channels);
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
                        for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
                            crsf_channels[i] = CRSF_CHANNEL_MID;
                        }
                        crsf_channels[2] = CRSF_CHANNEL_MIN;
                    }
                    gpio_put(LED_PIN, (now / 250) % 2);
                    break;

                default:
                    break;
            }

            size_t len = pb_payload
                ? crsf_build_frame_from_payload(pb_payload, crsf_packet)
                : crsf_build_rc_channels_packet(crsf_channels, crsf_packet);
            crsf_uart_send(crsf_packet, len);

            crsf_drain_telemetry();

            if (pb_payload) {
                if (++pb_frame_ctr >= REC_DECIMATION) {
                    pb_frame_ctr = 0;
                    if (++pb_sample_idx >= recorder_flash_sample_count()) {
                        pb_sample_idx = 0;
                        playback_running = false;
                        printf("# playback finished\n");
                    }
                }
            }

            dbg_stream_frame();

            if (dbg_snapshot_request) {
                dbg_snapshot_request = false;
                dbg_dump_snapshot(crsf_packet, len);
            }

            debug_log_channels();
        }
    }

    return 0;
}

#elif defined(BUILD_PI4)

// 標準入力をcbreakモード(ICANON/ECHO off, ノンブロッキング)に設定する。
// SSH越しの対話操作でも、Enter無しの単発キー(d/s/p/i/m/?)を受け付けるため。
// 標準入力がTTYでない場合(systemdサービス等)は何もせず、コマンドは無効のまま続行する。
static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void restore_stdin_termios(void) {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
}

static void init_stdin_rawmode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) {
        return;  // TTYでない
    }
    g_termios_saved = true;
    atexit(restore_stdin_termios);

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

static volatile sig_atomic_t g_running = 1;
static void handle_stop_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static uint32_t pi4_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

int main(void) {
    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);
    init_stdin_rawmode();

    printf("\n");
    printf("========================================\n");
    printf("  ExpressLRS Realtime Recorder (Raspberry Pi 4)\n");
    printf("========================================\n");
    printf("\n");

    init_channels();
    init_crsf_uart();
    recorder_init();
    usb_gamepad_init();

    printf("Mode: %s / Input: %s\n", mode_name(current_mode), input_name(current_input));
    printf("Commands: d=snapshot s=stream p=play/stop i=input-profile m=cycle-mode ?=help"
           "  (Ctrl+C to quit)\n\n");

    uint8_t crsf_packet[CRSF_MAX_PACKET_SIZE];
    bool rec_full_warned = false;

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    while (g_running) {
        usb_gamepad_task();
        dbg_poll_commands();

        uint32_t now = pi4_now_ms();
        const gamepad_state_t *gamepad = usb_gamepad_get_state();
        const uint8_t *pb_payload = NULL;

        switch (current_mode) {
            case MODE_SIMPLE:
            case MODE_RECORD:
                if (gamepad->connected) {
                    if (current_input == INPUT_LITERADIO && gamepad->channel_count >= 4) {
                        map_radio_to_channels(gamepad);
                    } else {
                        map_gamepad_to_channels(gamepad, now);
                    }
                } else {
                    set_failsafe_channels();
                }
                if (current_mode == MODE_RECORD) {
                    recorder_on_frame(crsf_channels);
                    if (recorder_is_full() && !rec_full_warned) {
                        rec_full_warned = true;
                        printf("# recording buffer FULL - press 'm' to stop and save\n");
                    }
                } else {
                    rec_full_warned = false;
                }
                break;

            case MODE_PLAYBACK:
                if (playback_running) {
                    pb_payload = recorder_flash_sample(pb_sample_idx);
                }
                if (pb_payload == NULL) {
                    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
                        crsf_channels[i] = CRSF_CHANNEL_MID;
                    }
                    crsf_channels[2] = CRSF_CHANNEL_MIN;
                }
                break;

            default:
                break;
        }

        size_t len = pb_payload
            ? crsf_build_frame_from_payload(pb_payload, crsf_packet)
            : crsf_build_rc_channels_packet(crsf_channels, crsf_packet);
        crsf_uart_send(crsf_packet, len);
        crsf_drain_telemetry();

        if (pb_payload) {
            if (++pb_frame_ctr >= REC_DECIMATION) {
                pb_frame_ctr = 0;
                if (++pb_sample_idx >= recorder_flash_sample_count()) {
                    pb_sample_idx = 0;
                    playback_running = false;
                    printf("# playback finished\n");
                }
            }
        }

        dbg_stream_frame();

        if (dbg_snapshot_request) {
            dbg_snapshot_request = false;
            dbg_dump_snapshot(crsf_packet, len);
        }

        debug_log_channels();

        next_tick.tv_nsec += (long)CRSF_SEND_INTERVAL_MS * 1000000L;
        while (next_tick.tv_nsec >= 1000000000L) {
            next_tick.tv_nsec -= 1000000000L;
            next_tick.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
    }

    printf("\n# shutting down\n");
    if (current_mode == MODE_RECORD) {
        recorder_stop_and_flush();
    }
    if (crsf_fd >= 0) close(crsf_fd);
    return 0;
}

#endif

#else
// ========================================
// ホストPC用スタブ（テスト用）
// ========================================

int main() {
    printf("ExpressLRS Realtime Recorder\n");
    printf("This is a Pico/Raspberry Pi 4 application.\n");
    printf("\n");
    printf("To build for Pico:\n");
    printf("  export PICO_SDK_PATH=/path/to/pico-sdk\n");
    printf("  mkdir build && cd build\n");
    printf("  cmake ..\n");
    printf("  make\n");
    printf("\n");
    printf("To build for Raspberry Pi 4 (run directly on the Pi):\n");
    printf("  mkdir build && cd build\n");
    printf("  cmake -DBUILD_PI4=ON ..\n");
    printf("  make\n");
    return 0;
}

#endif // PICO_BOARD / BUILD_PI4
