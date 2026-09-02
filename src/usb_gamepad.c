#include "usb_gamepad.h"
#include "hid_parser.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(PICO_BOARD) || defined(BUILD_PI4)
// ========================================
// 共有状態 / レポート解析（実機バックエンド共通）
// ========================================
// TinyUSB(Pico)版とhidraw(Pi4)版のどちらも同じ状態・パーサを使う。
// VID/PID専用パーサ（DS4/F310）とディスクリプタ解析結果を使う汎用パーサは
// プラットフォーム非依存なのでここに置く。

static gamepad_state_t gamepad_state = {0};
static gamepad_callback_t state_callback = NULL;

// レポートディスクリプタから求めたレイアウト。VID/PID専用パーサが無い
// デバイス（LiteRadio 3 等）はこれで読む。
static hid_layout_t hid_layout;

// 最後に受信した生HIDレポート（デバッグ/値確認用）
static uint8_t last_raw_report[64];
static uint16_t last_raw_len = 0;

// 解析したレイアウトをログに出す。未知のゲームパッドを繋いだとき、
// どの軸がどこに割り当たったかを実機で確認できるようにする。
static void log_hid_layout(const hid_layout_t *l) {
    static const char *axis_names[GAMEPAD_MAX_AXES] = {
        "LX", "LY", "RX", "RY", "L2", "R2", "A6", "A7"
    };
    if (!l->valid) {
        printf("# HID layout: not usable (falling back to fixed byte offsets)\n");
        return;
    }
    printf("# HID layout: report_id=%u buttons=%u@bit%u hat=%s\n",
           l->report_id, l->button_count, l->button_bit_offset,
           l->hat.present ? "yes" : "no");
    for (int a = 0; a < GAMEPAD_MAX_AXES; a++) {
        if (!l->axes[a].present) continue;
        printf("#   %s: bit%u size%u range %ld..%ld\n",
               axis_names[a], l->axes[a].bit_offset, l->axes[a].bit_size,
               (long)l->axes[a].logical_min, (long)l->axes[a].logical_max);
    }
}

// 汎用ゲームパッドのHIDレポート解析（ディスクリプタが読めない機種の最終手段）
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
// ボタンの物理⇔論理対応は Dual Action の慣例配置（1=X, 2=A, 3=B, 4=Y）。
// 実機確認済み(2026-08-06): A→CH7/AUX3, B→CH8/AUX4, X→CH9/AUX5, Y→CH10/AUX6,
// LB→CH11/AUX7, RB→CH12/AUX8 がBetaflight Receiverタブで全て一致。
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

// VID/PID専用パーサ or ディスクリプタ解析結果 or 最終手段、の優先順で1本の
// 生レポートを解析する（Pico/Pi4共通の振り分けロジック）。
static void dispatch_report(uint8_t const *report, uint16_t len) {
    uint16_t copy_len = (len > sizeof(last_raw_report)) ? sizeof(last_raw_report) : len;
    memcpy(last_raw_report, report, copy_len);
    last_raw_len = copy_len;

    if (gamepad_state.vid == 0x054C &&
        (gamepad_state.pid == 0x09CC || gamepad_state.pid == 0x05C4)) {
        process_ds4_report(report, len);
    } else if (gamepad_state.vid == 0x046D && gamepad_state.pid == 0xC216) {
        process_f310_report(report, len);
    } else if (hid_layout.valid &&
               hid_extract_state(&hid_layout, report, len, &gamepad_state)) {
        if (state_callback) {
            state_callback(&gamepad_state);
        }
    } else {
        process_gamepad_report(report, len);
    }
}

#endif // PICO_BOARD || BUILD_PI4

#if defined(PICO_BOARD)
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
// 解析したレイアウトをUARTに出す。未知のゲームパッドを繋いだとき、
// どの軸がどこに割り当たったかを実機で確認できるようにする。
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    first_report_logged = false;

    gamepad_state.connected = true;
    gamepad_state.vid = vid;
    gamepad_state.pid = pid;

    printf("# HID mounted: addr=%u inst=%u VID=%04X PID=%04X proto=%u desc_len=%u\n",
           dev_addr, instance, vid, pid,
           tuh_hid_interface_protocol(dev_addr, instance), desc_len);

    // レポートディスクリプタを解析しておく（VID/PID専用パーサが無い機種用）
    hid_parse_report_descriptor(desc_report, desc_len, &hid_layout);
    log_hid_layout(&hid_layout);

    // レポート受信を開始
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("# HID receive_report FAILED (addr=%u inst=%u)\n", dev_addr, instance);
    }
}

// TinyUSB コールバック: HIDデバイスがアンマウントされた
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("# HID unmounted: addr=%u inst=%u\n", dev_addr, instance);

    gamepad_state.connected = false;
    // 別の機種を挿し直したときに前のレイアウトで誤読しないようクリア
    hid_layout.valid = false;
}

// TinyUSB コールバック: HIDレポートを受信した
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    // 最初の1本だけレポート長を通知（レポートが届いているかの確認用）
    if (!first_report_logged) {
        first_report_logged = true;
        printf("# HID first report: len=%u\n", len);
    }

    dispatch_report(report, len);

    // 次のレポートを受信
    tuh_hid_receive_report(dev_addr, instance);
}

#elif defined(BUILD_PI4)
// ========================================
// Raspberry Pi 4 (Linux hidraw) 実装
// ========================================
// USBゲームパッドは Pi4 のUSBポートに直接接続する。TinyUSB(組込みUSBホスト
// スタック)の代わりに、Linuxカーネルのhidrawインターフェースから生レポート
// を読む。レポートディスクリプタもhidrawのioctlで取得でき、hid_parser.cに
// そのまま渡せるため、Pico版のパーサ資産をほぼ丸ごと再利用できる。

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

static int hid_fd = -1;

// hidrawデバイス1本を開き、ゲームパッドとして使えるか判定する。
// 判定基準: VID/PID専用パーサ対象（DS4/F310）、またはディスクリプタ解析で
// 軸が1つ以上見つかること。
static bool try_open_hidraw(const char *path) {
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    struct hidraw_devinfo info;
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        close(fd);
        return false;
    }
    uint16_t vid = (uint16_t)info.vendor;
    uint16_t pid = (uint16_t)info.product;

    int desc_size = 0;
    if (ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) < 0) {
        close(fd);
        return false;
    }
    struct hidraw_report_descriptor rdesc;
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.size = (uint32_t)desc_size;
    if (ioctl(fd, HIDIOCGRDESC, &rdesc) < 0) {
        close(fd);
        return false;
    }

    hid_layout_t layout;
    bool parsed = hid_parse_report_descriptor(rdesc.value, (uint16_t)rdesc.size, &layout);

    bool known_vid_pid =
        (vid == 0x054C && (pid == 0x09CC || pid == 0x05C4)) ||  // DualShock 4
        (vid == 0x046D && pid == 0xC216);                       // Logitech F310

    if (!known_vid_pid && !parsed) {
        // ゲームパッドとして使えなさそうなHID機器（マウス/キーボード等）
        close(fd);
        return false;
    }

    hid_fd = fd;
    hid_layout = parsed ? layout : (hid_layout_t){0};

    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) gamepad_state.axes[i] = 0;
    gamepad_state.buttons = 0;
    gamepad_state.channel_count = 0;
    gamepad_state.connected = true;
    gamepad_state.vid = vid;
    gamepad_state.pid = pid;

    printf("# gamepad opened: %s VID=%04X PID=%04X (%s)\n",
           path, vid, pid,
           known_vid_pid ? "known device, dedicated parser" : "descriptor parsed");
    if (parsed) {
        log_hid_layout(&hid_layout);
    }
    return true;
}

// 接続中のゲームパッドを探す。環境変数 GAMEPAD_HIDRAW で明示指定も可能
// （複数のHID機器が挿さっている場合の切り分け用）。
void usb_gamepad_init(void) {
    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) gamepad_state.axes[i] = 0;
    gamepad_state.buttons = 0;
    gamepad_state.connected = false;
    gamepad_state.vid = 0;
    gamepad_state.pid = 0;
    hid_layout.valid = false;
    hid_fd = -1;

    const char *forced = getenv("GAMEPAD_HIDRAW");
    if (forced) {
        if (!try_open_hidraw(forced)) {
            printf("# GAMEPAD_HIDRAW=%s: open failed (permission? wrong device?)\n", forced);
        }
        return;
    }

    char path[32];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/hidraw%d", i);
        if (try_open_hidraw(path)) return;
    }
    printf("# no gamepad found on /dev/hidraw0..15 (will keep retrying)\n");
}

void usb_gamepad_task(void) {
    if (hid_fd < 0) {
        // 毎ループ(2ms)の再走査は無駄なので間引く（約500ms毎）
        static int rescan_counter = 0;
        if (++rescan_counter >= 250) {
            rescan_counter = 0;
            usb_gamepad_init();
        }
        return;
    }

    uint8_t report[64];
    for (;;) {
        ssize_t n = read(hid_fd, report, sizeof(report));
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                printf("# gamepad disconnected\n");
                close(hid_fd);
                hid_fd = -1;
                gamepad_state.connected = false;
                hid_layout.valid = false;
            }
            break;
        }
        dispatch_report(report, (uint16_t)n);
    }
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

#endif // PICO_BOARD / BUILD_PI4

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
