#include "hid_parser.h"
#include <string.h>

// ========================================
// HIDレポートディスクリプタ解析
// ========================================
// ディスクリプタは「アイテム」の並び。各アイテムは
//   [prefix][data...]   prefix = tag(4bit) | type(2bit) | size(2bit)
// で、size 0/1/2/3 はデータ 0/1/2/4 バイトを意味する。
//
// 必要なのは Input アイテムに至るまでに積み上がった Global/Local の状態:
//   Global: Usage Page, Logical Min/Max, Report Size, Report Count, Report ID
//   Local : Usage, Usage Min/Max（そのMainアイテムの後にクリアされる）

#define ITEM_TYPE_MAIN    0
#define ITEM_TYPE_GLOBAL  1
#define ITEM_TYPE_LOCAL   2

#define MAIN_INPUT        0x8
#define MAIN_COLLECTION   0xA
#define MAIN_END_COLL     0xC

#define GLOBAL_USAGE_PAGE   0x0
#define GLOBAL_LOGICAL_MIN  0x1
#define GLOBAL_LOGICAL_MAX  0x2
#define GLOBAL_REPORT_SIZE  0x7
#define GLOBAL_REPORT_ID    0x8
#define GLOBAL_REPORT_COUNT 0x9

#define LOCAL_USAGE       0x0
#define LOCAL_USAGE_MIN   0x1
#define LOCAL_USAGE_MAX   0x2

#define USAGE_PAGE_DESKTOP  0x01
#define USAGE_PAGE_BUTTON   0x09

// Generic Desktop の軸 Usage
#define USAGE_X       0x30
#define USAGE_Y       0x31
#define USAGE_Z       0x32
#define USAGE_RX      0x33
#define USAGE_RY      0x34
#define USAGE_RZ      0x35
#define USAGE_SLIDER  0x36
#define USAGE_DIAL    0x37
#define USAGE_WHEEL   0x38
#define USAGE_HAT     0x39

// Input アイテムのビット1 = Constant（パディング。実データではない）
#define INPUT_CONSTANT  0x01

#define MAX_LOCAL_USAGES  16

// 汎用ボタンはビット0から順に詰めるが、ビット11〜14は D-Pad 用に
// 予約されている（usb_gamepad.h の GAMEPAD_BTN_DPAD_*）。ハットと
// 衝突させないため、取り込むのはビット0〜10の11個までとする。
// CRSFに載せるのは先頭6個（CH7〜CH12）なので実用上の不足はない。
#define GENERIC_BUTTON_LIMIT  11

// 解析中に見つけた軸を Usage 番号で一旦受けておく置き場。
// 全アイテムを見終わってから LX/LY/RX/... へ割り付ける。
typedef struct {
    hid_field_t by_usage[USAGE_HAT + 1];  // 0x30〜0x39 のみ使う
} axis_pool_t;

static uint32_t item_data(const uint8_t *p, uint8_t size) {
    switch (size) {
        case 1:  return p[0];
        case 2:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        case 3:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        default: return 0;
    }
}

// Logical Min/Max は符号付き。データ長に応じて符号拡張する。
static int32_t item_data_signed(const uint8_t *p, uint8_t size) {
    uint32_t v = item_data(p, size);
    switch (size) {
        case 1:  return (int32_t)(int8_t)v;
        case 2:  return (int32_t)(int16_t)v;
        case 3:  return (int32_t)v;
        default: return 0;
    }
}

// Z/Rz が無く Rx/Ry で右スティックを表す機種（ラジオ送信機に多い）に対応するため、
// 実際に存在する Usage を見てから割り付ける。
static void assign_axes(const axis_pool_t *pool, hid_layout_t *out) {
    const hid_field_t *x  = &pool->by_usage[USAGE_X];
    const hid_field_t *y  = &pool->by_usage[USAGE_Y];
    const hid_field_t *z  = &pool->by_usage[USAGE_Z];
    const hid_field_t *rx = &pool->by_usage[USAGE_RX];
    const hid_field_t *ry = &pool->by_usage[USAGE_RY];
    const hid_field_t *rz = &pool->by_usage[USAGE_RZ];

    if (x->present) out->axes[GAMEPAD_AXIS_LX] = *x;
    if (y->present) out->axes[GAMEPAD_AXIS_LY] = *y;

    // 右スティックX: Z が定番。無ければ Rx で代用。
    bool rx_used_as_stick = false;
    if (z->present) {
        out->axes[GAMEPAD_AXIS_RX] = *z;
    } else if (rx->present) {
        out->axes[GAMEPAD_AXIS_RX] = *rx;
        rx_used_as_stick = true;
    }

    // 右スティックY: Rz が定番。無ければ Ry で代用。
    bool ry_used_as_stick = false;
    if (rz->present) {
        out->axes[GAMEPAD_AXIS_RY] = *rz;
    } else if (ry->present) {
        out->axes[GAMEPAD_AXIS_RY] = *ry;
        ry_used_as_stick = true;
    }

    // 余った Rx/Ry はトリガー（Xbox系の定番配置）
    if (rx->present && !rx_used_as_stick) out->axes[GAMEPAD_AXIS_L2] = *rx;
    if (ry->present && !ry_used_as_stick) out->axes[GAMEPAD_AXIS_R2] = *ry;

    // 予備軸（送信機の補助チャンネル等）
    if (pool->by_usage[USAGE_SLIDER].present && GAMEPAD_MAX_AXES > 6) {
        out->axes[6] = pool->by_usage[USAGE_SLIDER];
    }
    if (pool->by_usage[USAGE_DIAL].present && GAMEPAD_MAX_AXES > 7) {
        out->axes[7] = pool->by_usage[USAGE_DIAL];
    }

    out->hat = pool->by_usage[USAGE_HAT];
}

bool hid_parse_report_descriptor(const uint8_t *desc, uint16_t len, hid_layout_t *out) {
    if (desc == NULL || out == NULL) return false;

    memset(out, 0, sizeof(*out));

    axis_pool_t pool;
    memset(&pool, 0, sizeof(pool));

    // Global/Local の現在値
    uint16_t usage_page = 0;
    int32_t  logical_min = 0, logical_max = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t  report_id = 0;
    uint32_t usages[MAX_LOCAL_USAGES];
    uint8_t  usage_count = 0;
    bool     usage_range = false;

    // ビット位置はレポートIDごとに独立して積み上がる。
    // 軸を含む最初のレポートIDだけを対象にする（ゲームパッドは通常1つ）。
    uint16_t bit_offset = 0;
    uint8_t  target_report_id = 0;
    bool     target_locked = false;

    uint16_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i++];

        if (prefix == 0xFE) {  // ロングアイテム: 使われないので読み飛ばす
            if (i >= len) break;
            uint8_t data_size = desc[i];
            i += 2 + data_size;
            continue;
        }

        uint8_t tag  = (prefix >> 4) & 0x0F;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t size = prefix & 0x03;
        uint8_t nbytes = (size == 3) ? 4 : size;

        if (i + nbytes > len) break;  // 壊れたディスクリプタ
        const uint8_t *data = &desc[i];
        i += nbytes;

        if (type == ITEM_TYPE_GLOBAL) {
            switch (tag) {
                case GLOBAL_USAGE_PAGE:   usage_page  = (uint16_t)item_data(data, size); break;
                case GLOBAL_LOGICAL_MIN:  logical_min = item_data_signed(data, size);    break;
                case GLOBAL_LOGICAL_MAX:  logical_max = item_data_signed(data, size);    break;
                case GLOBAL_REPORT_SIZE:  report_size  = item_data(data, size);          break;
                case GLOBAL_REPORT_COUNT: report_count = item_data(data, size);          break;
                case GLOBAL_REPORT_ID:
                    report_id = (uint8_t)item_data(data, size);
                    // レポートIDが変わればビット位置は先頭に戻る
                    if (!target_locked) {
                        target_report_id = report_id;
                        bit_offset = 0;
                    } else if (report_id != target_report_id) {
                        bit_offset = 0;
                    }
                    break;
                default: break;
            }
            continue;
        }

        if (type == ITEM_TYPE_LOCAL) {
            switch (tag) {
                case LOCAL_USAGE:
                    if (usage_count < MAX_LOCAL_USAGES) {
                        usages[usage_count++] = item_data(data, size);
                    }
                    break;
                case LOCAL_USAGE_MIN:
                case LOCAL_USAGE_MAX:
                    usage_range = true;  // ボタン列の指定。個別Usageは使わない。
                    break;
                default: break;
            }
            continue;
        }

        // --- Main アイテム ---
        if (type == ITEM_TYPE_MAIN) {
            if (tag == MAIN_INPUT) {
                uint32_t flags = item_data(data, size);
                bool constant = (flags & INPUT_CONSTANT) != 0;

                // 対象レポートID以外はビット位置だけ進めて中身は見ない
                bool collecting = (!target_locked) || (report_id == target_report_id);

                if (!constant && collecting) {
                    if (usage_page == USAGE_PAGE_BUTTON) {
                        // ボタンは連続ビット列。最初の1組だけ採用する。
                        if (out->button_count == 0 && report_size == 1) {
                            out->button_bit_offset = bit_offset;
                            out->button_count = (report_count > GENERIC_BUTTON_LIMIT)
                                                ? GENERIC_BUTTON_LIMIT
                                                : (uint8_t)report_count;
                            if (!target_locked) {
                                target_locked = true;
                                target_report_id = report_id;
                            }
                        }
                    } else if (usage_page == USAGE_PAGE_DESKTOP && !usage_range) {
                        // 軸・ハット。Usageは1フィールドにつき1つ対応する。
                        for (uint32_t f = 0; f < report_count; f++) {
                            uint32_t u = (f < usage_count) ? usages[f]
                                       : (usage_count > 0 ? usages[usage_count - 1] : 0);
                            if (u >= USAGE_X && u <= USAGE_HAT) {
                                hid_field_t *slot = &pool.by_usage[u];
                                if (!slot->present) {
                                    slot->present     = true;
                                    slot->bit_offset  = bit_offset + (uint16_t)(f * report_size);
                                    slot->bit_size    = (uint8_t)report_size;
                                    slot->logical_min = logical_min;
                                    slot->logical_max = logical_max;
                                    if (!target_locked) {
                                        target_locked = true;
                                        target_report_id = report_id;
                                    }
                                }
                            }
                        }
                    }
                }

                bit_offset += (uint16_t)(report_size * report_count);
            }

            // Main アイテムを処理したら Local はクリアされる（HID仕様）
            usage_count = 0;
            usage_range = false;
        }
    }

    assign_axes(&pool, out);
    out->report_id = target_report_id;

    for (int a = 0; a < GAMEPAD_MAX_AXES; a++) {
        if (out->axes[a].present) {
            out->valid = true;
            break;
        }
    }
    return out->valid;
}

// ========================================
// レポートからの値取り出し
// ========================================

// HIDのフィールドはバイト内をLSB側から詰め、バイトをまたいで連続する。
static uint32_t extract_bits(const uint8_t *report, uint16_t len,
                             uint16_t bit_offset, uint8_t bit_size) {
    uint32_t value = 0;
    for (uint8_t b = 0; b < bit_size && b < 32; b++) {
        uint16_t bit = bit_offset + b;
        uint16_t byte_idx = bit >> 3;
        if (byte_idx >= len) break;
        if (report[byte_idx] & (1u << (bit & 7))) {
            value |= (1u << b);
        }
    }
    return value;
}

// 論理範囲を -32768〜32767 に線形変換する。
static int16_t normalize_field(const hid_field_t *f, uint32_t raw) {
    int32_t v;
    if (f->logical_min < 0 && f->bit_size < 32) {
        // 符号付きフィールドは符号拡張する
        uint32_t sign_bit = 1u << (f->bit_size - 1);
        v = (raw & sign_bit) ? (int32_t)(raw | (~0u << f->bit_size)) : (int32_t)raw;
    } else {
        v = (int32_t)raw;
    }

    int32_t lo = f->logical_min, hi = f->logical_max;
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;

    int64_t span = (int64_t)hi - lo;
    int64_t scaled = ((int64_t)(v - lo) * 65535) / span - 32768;
    if (scaled < -32768) scaled = -32768;
    if (scaled > 32767)  scaled = 32767;
    return (int16_t)scaled;
}

bool hid_extract_state(const hid_layout_t *layout, const uint8_t *report,
                       uint16_t len, gamepad_state_t *state) {
    if (layout == NULL || report == NULL || state == NULL) return false;
    if (!layout->valid || len == 0) return false;

    // レポートIDがある場合、先頭バイトがID。一致しないレポートは対象外。
    if (layout->report_id != 0) {
        if (report[0] != layout->report_id) return false;
        report++;
        len--;
        if (len == 0) return false;
    }

    for (int a = 0; a < GAMEPAD_MAX_AXES; a++) {
        const hid_field_t *f = &layout->axes[a];
        if (!f->present) continue;
        state->axes[a] = normalize_field(f, extract_bits(report, len,
                                                         f->bit_offset, f->bit_size));
    }

    uint16_t buttons = 0;
    for (uint8_t b = 0; b < layout->button_count && b < GENERIC_BUTTON_LIMIT; b++) {
        if (extract_bits(report, len, layout->button_bit_offset + b, 1)) {
            buttons |= (uint16_t)(1u << b);
        }
    }

    // ハットスイッチ → D-Padボタン。論理値を 0=N..7=NW, 範囲外=中立 に直す。
    if (layout->hat.present) {
        uint32_t raw = extract_bits(report, len, layout->hat.bit_offset,
                                    layout->hat.bit_size);
        int32_t hv = (int32_t)raw - layout->hat.logical_min;
        if (hv >= 0 && hv <= 7) {
            buttons |= gamepad_dpad_to_buttons((uint8_t)hv);
        }
    }

    state->buttons = buttons;
    return true;
}
