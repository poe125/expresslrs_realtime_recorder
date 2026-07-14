#ifndef RECORDER_H
#define RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "crsf.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========================================
// 操作記録 / 再生
// ========================================
// 500Hzの送信フレームを 1/REC_DECIMATION に間引いて 50Hz で記録する。
// 記録中はRAMバッファに蓄積し、記録停止時にまとめてフラッシュへ書き出す
// （RP2040はフラッシュ書込み中XIPが停止するため、飛行中は書かない）。

// 間引き: 500Hz / 10 = 50Hz
#define REC_DECIMATION      10
// 1サンプル = パック済み22バイト payload
#define REC_SAMPLE_BYTES    CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE
// RAMバッファに保持する最大サンプル数（50Hzで約120秒）
#define REC_MAX_SAMPLES     6000
// 記録レート(Hz)。ヘッダ記載用。
#define REC_RATE_HZ         (1000 / (REC_DECIMATION * 2))  // = 50

// スイッチ(ボタン)系チャンネルのビットマスク（bitN = chN がスイッチ）。
// 現行マッピング: CH7-12 = index 6..11 をON/OFFスイッチとして扱う。
// スイッチchは間引き窓内で最大値ホールドし、一瞬の押下を取りこぼさない。
#define REC_SWITCH_CH_MASK  0x0FC0u   // bit6..bit11

// --- 記録側 ---
void     recorder_init(void);
void     recorder_start(void);              // バッファをクリアし記録開始
size_t   recorder_stop_and_flush(void);     // 記録停止＋RAM→フラッシュ。書いたサンプル数を返す
void     recorder_on_frame(const uint16_t channels[CRSF_NUM_CHANNELS]); // 500Hzフレームごとに呼ぶ
bool     recorder_is_active(void);
bool     recorder_is_full(void);
uint32_t recorder_ram_sample_count(void);
const uint8_t* recorder_ram_sample(uint32_t idx);  // RAM上のidx番サンプル(22B)。範囲外NULL

// --- 再生側（フラッシュ読出）---
uint32_t recorder_flash_sample_count(void);         // フラッシュ内の有効サンプル数（無効なら0）
const uint8_t* recorder_flash_sample(uint32_t idx); // フラッシュXIP上のidx番サンプル(22B)。範囲外NULL

#ifdef __cplusplus
}
#endif

#endif // RECORDER_H
