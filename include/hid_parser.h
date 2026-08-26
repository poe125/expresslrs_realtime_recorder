#ifndef HID_PARSER_H
#define HID_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include "usb_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

// HIDレポートディスクリプタを解析して、レポート中の軸/ボタンの位置を求める。
//
// 目的: VID/PIDごとにバイト位置を決め打ちする方式では、手元に無いデバイス
// （LiteRadio 3 等）に対応できない。ディスクリプタは全てのHIDデバイスが
// 自己申告するため、これを解析すれば未知のゲームパッドでも正しく読める。

// レポート内の1フィールド（1軸）の在り処
typedef struct {
    bool     present;
    uint16_t bit_offset;   // レポート先頭（レポートIDがあればその次）からのビット位置
    uint8_t  bit_size;
    int32_t  logical_min;
    int32_t  logical_max;
} hid_field_t;

// 解析結果。1つのレポートIDぶんのレイアウト。
typedef struct {
    bool        valid;              // 軸が1つ以上見つかったか
    uint8_t     report_id;          // 0 = レポートIDなし（先頭バイトから本体）
    hid_field_t axes[GAMEPAD_MAX_AXES];
    uint16_t    button_bit_offset;
    uint8_t     button_count;       // 0 = ボタン無し
    hid_field_t hat;                // ハットスイッチ（D-Pad）
} hid_layout_t;

/**
 * HIDレポートディスクリプタを解析する。
 *
 * 軸のマッピングは Generic Desktop の Usage から決める:
 *   X→LX, Y→LY, Z→RX, Rz→RY, Rx→L2, Ry→R2
 * ただし Z/Rz が無く Rx/Ry がある機種（ラジオ送信機に多い）では
 * Rx→RX, Ry→RY として右スティックに割り当てる。
 *
 * @return 軸が1つ以上見つかれば true
 */
bool hid_parse_report_descriptor(const uint8_t *desc, uint16_t len, hid_layout_t *out);

/**
 * 解析済みレイアウトを使って生レポートから軸・ボタンを取り出す。
 * axes は -32768〜32767 に正規化される。
 *
 * @return レポートが対象レポートIDと一致し、取り出せたら true
 */
bool hid_extract_state(const hid_layout_t *layout, const uint8_t *report,
                       uint16_t len, gamepad_state_t *state);

#ifdef __cplusplus
}
#endif

#endif // HID_PARSER_H
