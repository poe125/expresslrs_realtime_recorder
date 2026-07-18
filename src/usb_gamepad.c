#include "usb_gamepad.h"
#include <stddef.h>
#include <stdio.h>

#ifdef PICO_BOARD
// ========================================
// Pico SDK + TinyUSB Host 実装
// ========================================

#include "tusb.h"
#include "bsp/board_api.h"
#include "pico/time.h"

// TinyUSB(OS_NONE構成)が要求する時刻API。
// tusb.h で「API Implemented by user」とされ、RTOSなし構成では
// アプリ側で実装する必要がある（rp2040 BSPは未提供）。
uint32_t tusb_time_millis_api(void) {
    return to_ms_since_boot(get_absolute_time());
}

static gamepad_state_t gamepad_state = {0};
static gamepad_callback_t state_callback = NULL;

// HIDレポート解析用の一時バッファ
static uint8_t hid_dev_addr = 0;
static uint8_t hid_instance = 0;

// 最後に受信した生HIDレポート（デバッグ/値確認用）
static uint8_t last_raw_report[64];
static uint16_t last_raw_len = 0;

// マウント毎に1本目のレポート長を通知するためのフラグ（mount時にリセット）
static bool first_report_logged = false;

void usb_gamepad_init(void) {
    board_init();
    tusb_init();

    // 状態を初期化
    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) {
        gamepad_state.axes[i] = 0;
    }
    gamepad_state.buttons = 0;
    gamepad_state.connected = false;
    gamepad_state.vid = 0;
    gamepad_state.pid = 0;
}

void usb_gamepad_task(void) {
    tuh_task();
}

const gamepad_state_t* usb_gamepad_get_state(void) {
    return &gamepad_state;
}

void usb_gamepad_set_callback(gamepad_callback_t callback) {
    state_callback = callback;
}

bool usb_gamepad_is_connected(void) {
    return gamepad_state.connected;
}

uint16_t usb_gamepad_get_raw_report(const uint8_t **report_out) {
    if (report_out) {
        *report_out = last_raw_report;
    }
    return last_raw_len;
}

// 汎用ゲームパッドのHIDレポート解析
static void process_gamepad_report(uint8_t const *report, uint16_t len) {
    if (len < 4) return;

    // 一般的なゲームパッドレポート形式を想定
    // [0]: Left Stick X (0-255)
    // [1]: Left Stick Y (0-255)
    // [2]: Right Stick X (0-255)
    // [3]: Right Stick Y (0-255)
    // [4]: Buttons low byte
    // [5]: Buttons high byte (if available)
    // [6]: D-Pad (some controllers)

    gamepad_state.axes[GAMEPAD_AXIS_LX] = gamepad_axis_u8_to_s16(report[0]);
    gamepad_state.axes[GAMEPAD_AXIS_LY] = gamepad_axis_u8_to_s16(report[1]);

    if (len >= 4) {
        gamepad_state.axes[GAMEPAD_AXIS_RX] = gamepad_axis_u8_to_s16(report[2]);
        gamepad_state.axes[GAMEPAD_AXIS_RY] = gamepad_axis_u8_to_s16(report[3]);
    }

    if (len >= 5) {
        gamepad_state.buttons = report[4];
        if (len >= 6) {
            gamepad_state.buttons |= (report[5] << 8);
        }
    }

    // コールバックを呼び出し
    if (state_callback) {
        state_callback(&gamepad_state);
    }
}

// Sony DualShock 4 レポート解析
static void process_ds4_report(uint8_t const *report, uint16_t len) {
    if (len < 10) return;

    // DS4レポート形式
    // [0]: Report ID (0x01)
    // [1]: Left Stick X
    // [2]: Left Stick Y
    // [3]: Right Stick X
    // [4]: Right Stick Y
    // [5]: D-Pad + Buttons (Square, Cross, Circle, Triangle)
    // [6]: Buttons (L1, R1, L2, R2, Share, Options, L3, R3)
    // [7]: PS Button + Touchpad
    // [8]: L2 Trigger
    // [9]: R2 Trigger

    gamepad_state.axes[GAMEPAD_AXIS_LX] = gamepad_axis_u8_to_s16(report[1]);
    gamepad_state.axes[GAMEPAD_AXIS_LY] = gamepad_axis_u8_to_s16(report[2]);
    gamepad_state.axes[GAMEPAD_AXIS_RX] = gamepad_axis_u8_to_s16(report[3]);
    gamepad_state.axes[GAMEPAD_AXIS_RY] = gamepad_axis_u8_to_s16(report[4]);
    gamepad_state.axes[GAMEPAD_AXIS_L2] = gamepad_axis_u8_to_s16(report[8]);
    gamepad_state.axes[GAMEPAD_AXIS_R2] = gamepad_axis_u8_to_s16(report[9]);

    // ボタンを解析
    uint16_t buttons = 0;
    uint8_t dpad = report[5] & 0x0F;

    // D-Pad
    buttons |= gamepad_dpad_to_buttons(dpad);

    // Face buttons
    if (report[5] & 0x10) buttons |= GAMEPAD_BTN_X;      // Square
    if (report[5] & 0x20) buttons |= GAMEPAD_BTN_A;      // Cross
    if (report[5] & 0x40) buttons |= GAMEPAD_BTN_B;      // Circle
    if (report[5] & 0x80) buttons |= GAMEPAD_BTN_Y;      // Triangle

    // Shoulder buttons
    if (report[6] & 0x01) buttons |= GAMEPAD_BTN_LB;     // L1
    if (report[6] & 0x02) buttons |= GAMEPAD_BTN_RB;     // R1
    if (report[6] & 0x10) buttons |= GAMEPAD_BTN_BACK;   // Share
    if (report[6] & 0x20) buttons |= GAMEPAD_BTN_START;  // Options
    if (report[6] & 0x40) buttons |= GAMEPAD_BTN_L3;
    if (report[6] & 0x80) buttons |= GAMEPAD_BTN_R3;

    // PS Button
    if (report[7] & 0x01) buttons |= GAMEPAD_BTN_HOME;

    gamepad_state.buttons = buttons;

    if (state_callback) {
        state_callback(&gamepad_state);
    }
}

// Logitech F310 (Dモード / Dual Action) レポート解析
// レポート記述子(99B)の解読結果に基づく 8バイトレポート:
//   [0] Left Stick X  [1] Left Stick Y  [2] Right Stick X  [3] Right Stick Y
//   [4] 下位4bit=D-Pad(ハット 0-7, 8=中立), 上位4bit=ボタン1-4
//   [5] ボタン5-12    [6..7] ベンダー固有
// ⚠️ ボタンの物理⇔論理対応は Dual Action の慣例配置に基づく仮マッピング。
//    実機で各ボタンを押し `d` スナップショットで確認・必要なら修正すること(Stage C)。
static void process_f310_report(uint8_t const *report, uint16_t len) {
    if (len < 6) return;

    gamepad_state.axes[GAMEPAD_AXIS_LX] = gamepad_axis_u8_to_s16(report[0]);
    gamepad_state.axes[GAMEPAD_AXIS_LY] = gamepad_axis_u8_to_s16(report[1]);
    gamepad_state.axes[GAMEPAD_AXIS_RX] = gamepad_axis_u8_to_s16(report[2]);
    gamepad_state.axes[GAMEPAD_AXIS_RY] = gamepad_axis_u8_to_s16(report[3]);

    uint16_t buttons = 0;

    // D-Pad（ハット: byte4 下位4bit, 0-7=方向, 8=中立）
    buttons |= gamepad_dpad_to_buttons(report[4] & 0x0F);

    // フェイスボタン（byte4 上位4bit = ボタン1-4）
    if (report[4] & 0x10) buttons |= GAMEPAD_BTN_X;      // ボタン1
    if (report[4] & 0x20) buttons |= GAMEPAD_BTN_A;      // ボタン2
    if (report[4] & 0x40) buttons |= GAMEPAD_BTN_B;      // ボタン3
    if (report[4] & 0x80) buttons |= GAMEPAD_BTN_Y;      // ボタン4

    // ショルダー・その他（byte5 = ボタン5-12）
    if (report[5] & 0x01) buttons |= GAMEPAD_BTN_LB;     // ボタン5 (L1)
    if (report[5] & 0x02) buttons |= GAMEPAD_BTN_RB;     // ボタン6 (R1)
    // byte5 bit2/3 = L2/R2(F310はデジタル、CRSF未使用のため省略)
    if (report[5] & 0x10) buttons |= GAMEPAD_BTN_BACK;   // ボタン9 (Back)
    if (report[5] & 0x20) buttons |= GAMEPAD_BTN_START;  // ボタン10 (Start)
    if (report[5] & 0x40) buttons |= GAMEPAD_BTN_L3;     // ボタン11 (L3)
    if (report[5] & 0x80) buttons |= GAMEPAD_BTN_R3;     // ボタン12 (R3)

    gamepad_state.buttons = buttons;

    if (state_callback) {
        state_callback(&gamepad_state);
    }
}

// TinyUSB コールバック: USBデバイスが列挙された（HIDドライバより前の段階）
// HIDのmountが来ないのにこちらが来る場合、列挙は成功しHIDドライバで弾かれている。
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("# USB mounted: addr=%u VID=%04X PID=%04X\n", dev_addr, vid, pid);
}

// TinyUSB コールバック: USBデバイスが切断された
void tuh_umount_cb(uint8_t dev_addr) {
    printf("# USB unmounted: addr=%u\n", dev_addr);
}

// TinyUSB コールバック: HIDデバイスがマウントされた
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    hid_dev_addr = dev_addr;
    hid_instance = instance;
    first_report_logged = false;

    gamepad_state.connected = true;
    gamepad_state.vid = vid;
    gamepad_state.pid = pid;

    printf("# HID mounted: addr=%u inst=%u VID=%04X PID=%04X proto=%u desc_len=%u\n",
           dev_addr, instance, vid, pid,
           tuh_hid_interface_protocol(dev_addr, instance), desc_len);

    // レポート受信を開始
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("# HID receive_report FAILED (addr=%u inst=%u)\n", dev_addr, instance);
    }
}

// TinyUSB コールバック: HIDデバイスがアンマウントされた
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("# HID unmounted: addr=%u inst=%u\n", dev_addr, instance);

    gamepad_state.connected = false;
    hid_dev_addr = 0;
    hid_instance = 0;
}

// TinyUSB コールバック: HIDレポートを受信した
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    // 最初の1本だけレポート長を通知（レポートが届いているかの確認用）
    if (!first_report_logged) {
        first_report_logged = true;
        printf("# HID first report: len=%u\n", len);
    }

    // 生レポートを保存（デバッグ/値確認用）
    uint16_t copy_len = (len > sizeof(last_raw_report)) ? sizeof(last_raw_report) : len;
    for (uint16_t i = 0; i < copy_len; i++) {
        last_raw_report[i] = report[i];
    }
    last_raw_len = copy_len;

    // Sony DualShock 4 を検出
    if (gamepad_state.vid == 0x054C &&
        (gamepad_state.pid == 0x09CC || gamepad_state.pid == 0x05C4)) {
        process_ds4_report(report, len);
    } else if (gamepad_state.vid == 0x046D && gamepad_state.pid == 0xC216) {
        // Logitech F310 (Dモード / Dual Action)
        process_f310_report(report, len);
    } else {
        // 汎用ゲームパッドとして処理
        process_gamepad_report(report, len);
    }

    // 次のレポートを受信
    tuh_hid_receive_report(dev_addr, instance);
}

#else
// ========================================
// ホストPC用スタブ実装（テスト用）
// ========================================

static gamepad_state_t gamepad_state = {0};
static gamepad_callback_t state_callback = NULL;

void usb_gamepad_init(void) {
    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) {
        gamepad_state.axes[i] = 0;
    }
    gamepad_state.buttons = 0;
    gamepad_state.connected = false;
}

void usb_gamepad_task(void) {
    // スタブ: 何もしない
}

const gamepad_state_t* usb_gamepad_get_state(void) {
    return &gamepad_state;
}

void usb_gamepad_set_callback(gamepad_callback_t callback) {
    state_callback = callback;
}

bool usb_gamepad_is_connected(void) {
    return gamepad_state.connected;
}

uint16_t usb_gamepad_get_raw_report(const uint8_t **report_out) {
    // スタブ: 生レポートは保持しない
    if (report_out) {
        *report_out = NULL;
    }
    return 0;
}

// テスト用: ゲームパッド状態を設定
void usb_gamepad_test_set_state(const gamepad_state_t *state) {
    gamepad_state = *state;
    if (state_callback) {
        state_callback(&gamepad_state);
    }
}

#endif // PICO_BOARD

// ========================================
// 共通ユーティリティ関数
// ========================================

int16_t gamepad_axis_u8_to_s16(uint8_t value) {
    // 0-255 を -32768〜32767 に変換
    // 128 が中央（0）になるように
    int32_t temp = ((int32_t)value - 128) * 256;

    // クランプ
    if (temp < -32768) temp = -32768;
    if (temp > 32767) temp = 32767;

    return (int16_t)temp;
}

uint16_t gamepad_dpad_to_buttons(uint8_t dpad) {
    // D-Pad値:
    // 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=neutral
    uint16_t buttons = 0;

    switch (dpad) {
        case 0: // N
            buttons = GAMEPAD_BTN_DPAD_U;
            break;
        case 1: // NE
            buttons = GAMEPAD_BTN_DPAD_U | GAMEPAD_BTN_DPAD_R;
            break;
        case 2: // E
            buttons = GAMEPAD_BTN_DPAD_R;
            break;
        case 3: // SE
            buttons = GAMEPAD_BTN_DPAD_D | GAMEPAD_BTN_DPAD_R;
            break;
        case 4: // S
            buttons = GAMEPAD_BTN_DPAD_D;
            break;
        case 5: // SW
            buttons = GAMEPAD_BTN_DPAD_D | GAMEPAD_BTN_DPAD_L;
            break;
        case 6: // W
            buttons = GAMEPAD_BTN_DPAD_L;
            break;
        case 7: // NW
            buttons = GAMEPAD_BTN_DPAD_U | GAMEPAD_BTN_DPAD_L;
            break;
        default: // 8 = neutral or invalid
            buttons = 0;
            break;
    }

    return buttons;
}
