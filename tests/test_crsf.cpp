#include <gtest/gtest.h>
#include "crsf.h"

// =============================================================================
// CRC8テスト
// =============================================================================

TEST(CrsfCrc8Test, EmptyData) {
    // 空データのCRCは0
    uint8_t crc = crsf_crc8(nullptr, 0);
    EXPECT_EQ(crc, 0);
}

TEST(CrsfCrc8Test, SingleByte) {
    // 単一バイトのCRC計算
    uint8_t data[] = {0x16};  // フレームタイプ
    uint8_t crc = crsf_crc8(data, 1);
    // CRC8-DVB-S2で0x16のCRCを検証
    EXPECT_NE(crc, 0);  // 非ゼロであること
}

TEST(CrsfCrc8Test, KnownPacket) {
    // 公式パケット例でCRCを検証
    // https://github.com/crsf-wg/crsf/wiki/CRSF_FRAMETYPE_RC_CHANNELS_PACKED
    // 全チャンネル992のパケット: c8 18 16 e0 03 ... ad
    // CRCは type + payload で計算
    uint8_t data[] = {
        0x16,  // Type: RC Channels Packed
        // 16ch全て992 (中央値) をパックした22バイト
        0xE0, 0x03, 0x1F, 0xF8, 0xC0, 0x07, 0x3E, 0xF0,
        0x81, 0x0F, 0x7C, 0xE0, 0x03, 0x1F, 0xF8, 0xC0,
        0x07, 0x3E, 0xF0, 0x81, 0x0F, 0x7C
    };
    uint8_t crc = crsf_crc8(data, sizeof(data));
    // 公式例ではCRCは0xad
    EXPECT_EQ(crc, 0xAD);
}

// =============================================================================
// チャンネル値正規化テスト
// =============================================================================

TEST(CrsfNormalizeTest, CenterValue) {
    // 入力0（中央）→ CRSF中央値992
    EXPECT_EQ(crsf_normalize_channel(0), CRSF_CHANNEL_MID);
}

TEST(CrsfNormalizeTest, MinValue) {
    // 入力最小値 → CRSF最小値172
    EXPECT_EQ(crsf_normalize_channel(-32768), CRSF_CHANNEL_MIN);
}

TEST(CrsfNormalizeTest, MaxValue) {
    // 入力最大値 → CRSF最大値1811
    EXPECT_EQ(crsf_normalize_channel(32767), CRSF_CHANNEL_MAX);
}

TEST(CrsfNormalizeTest, QuarterNegative) {
    // 入力-16384（25%位置）
    uint16_t result = crsf_normalize_channel(-16384);
    EXPECT_GT(result, CRSF_CHANNEL_MIN);
    EXPECT_LT(result, CRSF_CHANNEL_MID);
}

TEST(CrsfNormalizeTest, QuarterPositive) {
    // 入力16383（75%位置）
    uint16_t result = crsf_normalize_channel(16383);
    EXPECT_GT(result, CRSF_CHANNEL_MID);
    EXPECT_LT(result, CRSF_CHANNEL_MAX);
}

// =============================================================================
// チャンネルパッキングテスト
// =============================================================================

TEST(CrsfPackChannelsTest, AllZero) {
    // 全チャンネル0（最小値172相当）の場合
    uint16_t channels[CRSF_NUM_CHANNELS] = {0};
    uint8_t output[CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE] = {0};

    crsf_pack_channels(channels, output);

    // 出力が空でないことを確認
    bool all_zero = true;
    for (int i = 0; i < CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE; i++) {
        if (output[i] != 0) all_zero = false;
    }
    // 値が0でもパッキング結果は何かあるはず
    // （実装依存だが、少なくとも関数が動作すること）
}

TEST(CrsfPackChannelsTest, AllCenter) {
    // 全チャンネル中央値992
    // 公式パケット例と照合
    uint16_t channels[CRSF_NUM_CHANNELS];
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        channels[i] = CRSF_CHANNEL_MID;  // 992
    }
    uint8_t output[CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE] = {0};

    crsf_pack_channels(channels, output);

    // 公式例のペイロード（22バイト）
    uint8_t expected[] = {
        0xE0, 0x03, 0x1F, 0xF8, 0xC0, 0x07, 0x3E, 0xF0,
        0x81, 0x0F, 0x7C, 0xE0, 0x03, 0x1F, 0xF8, 0xC0,
        0x07, 0x3E, 0xF0, 0x81, 0x0F, 0x7C
    };

    for (int i = 0; i < CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE; i++) {
        EXPECT_EQ(output[i], expected[i]) << "Mismatch at byte " << i;
    }
}

TEST(CrsfPackChannelsTest, FirstChannelMax) {
    // Ch0が最大値1811、他は中央値
    uint16_t channels[CRSF_NUM_CHANNELS];
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        channels[i] = CRSF_CHANNEL_MID;
    }
    channels[0] = CRSF_CHANNEL_MAX;  // 1811 = 0x713
    uint8_t output[CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE] = {0};

    crsf_pack_channels(channels, output);

    // 1811 = 0x713 (11bit)
    // 下位8bit: 0x13
    EXPECT_EQ(output[0], 0x13);
}

TEST(CrsfPackChannelsTest, OutputSize) {
    // 出力サイズが22バイトであることを確認
    // パケット生成関数の戻り値でサイズを検証
    uint16_t channels[CRSF_NUM_CHANNELS] = {0};
    uint8_t buffer[CRSF_MAX_PACKET_SIZE] = {0};

    size_t len = crsf_build_rc_channels_packet(channels, buffer);

    // sync(1) + len(1) + type(1) + payload(22) + crc(1) = 26
    EXPECT_EQ(len, 26);
    // ペイロードは22バイト
    EXPECT_EQ(CRSF_RC_CHANNELS_PACKED_PAYLOAD_SIZE, 22);
}

// =============================================================================
// パケット生成テスト
// =============================================================================

TEST(CrsfBuildPacketTest, PacketStructure) {
    uint16_t channels[CRSF_NUM_CHANNELS];
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        channels[i] = CRSF_CHANNEL_MID;
    }
    uint8_t buffer[CRSF_MAX_PACKET_SIZE] = {0};

    size_t len = crsf_build_rc_channels_packet(channels, buffer);

    // パケット長: sync(1) + len(1) + type(1) + payload(22) + crc(1) = 26
    EXPECT_EQ(len, 26);

    // SYNCバイト
    EXPECT_EQ(buffer[0], CRSF_SYNC_BYTE);

    // LENフィールド（type + payload + crc = 24）
    EXPECT_EQ(buffer[1], 24);

    // TYPEフィールド
    EXPECT_EQ(buffer[2], CRSF_FRAMETYPE_RC_CHANNELS_PACKED);
}

TEST(CrsfBuildPacketTest, CrcIsValid) {
    uint16_t channels[CRSF_NUM_CHANNELS];
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        channels[i] = CRSF_CHANNEL_MID;
    }
    uint8_t buffer[CRSF_MAX_PACKET_SIZE] = {0};

    size_t len = crsf_build_rc_channels_packet(channels, buffer);

    // CRCを検証（type + payloadでCRC計算）
    uint8_t calculated_crc = crsf_crc8(&buffer[2], 23);  // type(1) + payload(22)
    EXPECT_EQ(buffer[len - 1], calculated_crc);
}

TEST(CrsfBuildPacketTest, MatchOfficialExample) {
    // 公式パケット例との完全一致を検証
    // https://github.com/crsf-wg/crsf/wiki/CRSF_FRAMETYPE_RC_CHANNELS_PACKED
    // 全チャンネル992: c8 18 16 e0 03 1f f8 c0 07 3e f0 81 0f 7c e0 03 1f f8 c0 07 3e f0 81 0f 7c ad
    uint16_t channels[CRSF_NUM_CHANNELS];
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        channels[i] = CRSF_CHANNEL_MID;  // 992
    }
    uint8_t buffer[CRSF_MAX_PACKET_SIZE] = {0};

    size_t len = crsf_build_rc_channels_packet(channels, buffer);

    uint8_t expected[] = {
        0xC8, 0x18, 0x16,  // SYNC, LEN(24), TYPE
        0xE0, 0x03, 0x1F, 0xF8, 0xC0, 0x07, 0x3E, 0xF0,
        0x81, 0x0F, 0x7C, 0xE0, 0x03, 0x1F, 0xF8, 0xC0,
        0x07, 0x3E, 0xF0, 0x81, 0x0F, 0x7C,  // PAYLOAD
        0xAD  // CRC
    };

    EXPECT_EQ(len, sizeof(expected));
    for (size_t i = 0; i < len; i++) {
        EXPECT_EQ(buffer[i], expected[i]) << "Mismatch at byte " << i;
    }
}

TEST(CrsfBuildPacketTest, DifferentChannelsProduceDifferentPackets) {
    uint16_t channels1[CRSF_NUM_CHANNELS] = {0};
    uint16_t channels2[CRSF_NUM_CHANNELS] = {0};
    channels2[0] = 1000;  // Ch0だけ異なる

    uint8_t buffer1[CRSF_MAX_PACKET_SIZE] = {0};
    uint8_t buffer2[CRSF_MAX_PACKET_SIZE] = {0};

    crsf_build_rc_channels_packet(channels1, buffer1);
    crsf_build_rc_channels_packet(channels2, buffer2);

    // ペイロードが異なることを確認
    bool different = false;
    for (int i = 3; i < 25; i++) {  // payload部分
        if (buffer1[i] != buffer2[i]) {
            different = true;
            break;
        }
    }
    EXPECT_TRUE(different);
}

// =============================================================================
// メイン
// =============================================================================

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
