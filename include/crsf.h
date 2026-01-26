#ifndef CRSF_H
#define CRSF_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// CRSF定数
#define CRSF_SYNC_BYTE          0xC8
#define CRSF_FRAMETYPE_RC_CHANNELS_PACKED 0x16

// CRSFチャンネル値の範囲
#define CRSF_CHANNEL_MIN        172
#define CRSF_CHANNEL_MID        992
#define CRSF_CHANNEL_MAX        1811

// CRSFパケットサイズ
#define CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE 22
#define CRSF_MAX_PACKET_SIZE    64

// チャンネル数
#define CRSF_NUM_CHANNELS       16

// CRSFパケット構造体
typedef struct {
    uint8_t sync;
    uint8_t len;
    uint8_t type;
    uint8_t payload[CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE];
    uint8_t crc;
} crsf_rc_channels_packet_t;

// CRC8計算 (DVB-S2多項式 0xD5)
uint8_t crsf_crc8(const uint8_t *data, size_t len);

// チャンネル値を正規化（入力: -32768〜32767 → 出力: 172〜1811）
uint16_t crsf_normalize_channel(int16_t input);

// 16チャンネルの値を22バイトにパック（11bit x 16）
void crsf_pack_channels(const uint16_t channels[CRSF_NUM_CHANNELS], uint8_t *output);

// RC Channels Packedパケットを生成
size_t crsf_build_rc_channels_packet(const uint16_t channels[CRSF_NUM_CHANNELS], uint8_t *buffer);

#ifdef __cplusplus
}
#endif

#endif // CRSF_H
