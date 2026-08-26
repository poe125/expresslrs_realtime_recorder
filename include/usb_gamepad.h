#ifndef USB_GAMEPAD_H
#define USB_GAMEPAD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ゲームパッドの軸数とボタン数
#define GAMEPAD_MAX_AXES    8
#define GAMEPAD_MAX_BUTTONS 16

// ゲームパッドの状態
typedef struct {
    // アナログ軸（-32768 〜 32767）。ゲームパッドのスティック意味づけ。
    int16_t axes[GAMEPAD_MAX_AXES];
    // ラジオ送信機用: ディスクリプタ順（=チャンネル順 CH1,CH2,…）の生値。
    // 送信機はスティックではなくAETR等のチャンネル順で軸を並べるため、
    // スティックへ読み替えず素通しするための経路。
    // VID/PID専用パーサ（F310/DS4）使用時は channel_count = 0。
    int16_t channels[GAMEPAD_MAX_AXES];
    uint8_t channel_count;
    // ボタン状態（ビットフラグ）
    uint16_t buttons;
    // 接続状態
    bool connected;
    // VID/PID
    uint16_t vid;
    uint16_t pid;
} gamepad_state_t;

// 標準的な軸のインデックス
typedef enum {
    GAMEPAD_AXIS_LX = 0,  // 左スティック X
    GAMEPAD_AXIS_LY = 1,  // 左スティック Y
    GAMEPAD_AXIS_RX = 2,  // 右スティック X
    GAMEPAD_AXIS_RY = 3,  // 右スティック Y
    GAMEPAD_AXIS_L2 = 4,  // 左トリガー
    GAMEPAD_AXIS_R2 = 5,  // 右トリガー
} gamepad_axis_t;

// 標準的なボタンのビット位置
typedef enum {
    GAMEPAD_BTN_A      = (1 << 0),   // A / Cross
    GAMEPAD_BTN_B      = (1 << 1),   // B / Circle
    GAMEPAD_BTN_X      = (1 << 2),   // X / Square
    GAMEPAD_BTN_Y      = (1 << 3),   // Y / Triangle
    GAMEPAD_BTN_LB     = (1 << 4),   // Left Bumper / L1
    GAMEPAD_BTN_RB     = (1 << 5),   // Right Bumper / R1
    GAMEPAD_BTN_BACK   = (1 << 6),   // Back / Select / Share
    GAMEPAD_BTN_START  = (1 << 7),   // Start / Options
    GAMEPAD_BTN_L3     = (1 << 8),   // Left Stick Click
    GAMEPAD_BTN_R3     = (1 << 9),   // Right Stick Click
    GAMEPAD_BTN_HOME   = (1 << 10),  // Home / PS Button
    GAMEPAD_BTN_DPAD_U = (1 << 11),  // D-Pad Up
    GAMEPAD_BTN_DPAD_D = (1 << 12),  // D-Pad Down
    GAMEPAD_BTN_DPAD_L = (1 << 13),  // D-Pad Left
    GAMEPAD_BTN_DPAD_R = (1 << 14),  // D-Pad Right
} gamepad_button_t;

// コールバック関数型
typedef void (*gamepad_callback_t)(const gamepad_state_t *state);

// USB Gamepad Host API

/**
 * USB Gamepad Hostを初期化
 */
void usb_gamepad_init(void);

/**
 * USB Gamepad Hostのタスク処理（メインループで呼び出す）
 */
void usb_gamepad_task(void);

/**
 * 現在のゲームパッド状態を取得
 */
const gamepad_state_t* usb_gamepad_get_state(void);

/**
 * ゲームパッド状態変化時のコールバックを設定
 */
void usb_gamepad_set_callback(gamepad_callback_t callback);

/**
 * ゲームパッドが接続されているか
 */
bool usb_gamepad_is_connected(void);

/**
 * 最後に受信した生HIDレポートを取得（デバッグ/値確認用）
 * @param report_out レポート先頭へのポインタを格納（NULL可）
 * @return レポート長（バイト数）。未受信時は0。
 */
uint16_t usb_gamepad_get_raw_report(const uint8_t **report_out);

// ユーティリティ関数

/**
 * 8bit unsigned (0-255) を 16bit signed (-32768〜32767) に変換
 */
int16_t gamepad_axis_u8_to_s16(uint8_t value);

/**
 * D-Pad値（0-7, 8=neutral）をボタンビットに変換
 */
uint16_t gamepad_dpad_to_buttons(uint8_t dpad);

#ifdef __cplusplus
}
#endif

#endif // USB_GAMEPAD_H
