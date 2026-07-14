#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "recorder.h"
#include "crsf.h"
}

// 全ch中央、指定chだけ値を差し替えた入力を作るヘルパ
static void make_channels(uint16_t ch[CRSF_NUM_CHANNELS], int idx, uint16_t val) {
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) ch[i] = CRSF_CHANNEL_MID;
    if (idx >= 0) ch[idx] = val;
}

// 期待するchannelsをパックしたバイト列と、記録された生サンプルを比較
static void expect_sample_equals(uint32_t sample_idx, const uint16_t expected[CRSF_NUM_CHANNELS]) {
    uint8_t packed[REC_SAMPLE_BYTES];
    crsf_pack_channels(expected, packed);
    const uint8_t *got = recorder_ram_sample(sample_idx);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(0, memcmp(packed, got, REC_SAMPLE_BYTES));
}

// 10フレームで1サンプル確定（50Hz間引き）
TEST(RecorderTest, DecimationProducesOneSamplePerTenFrames) {
    recorder_start();
    uint16_t ch[CRSF_NUM_CHANNELS];
    make_channels(ch, -1, 0);

    for (int i = 0; i < 9; i++) {
        recorder_on_frame(ch);
        EXPECT_EQ(0u, recorder_ram_sample_count());  // 9フレームではまだ0
    }
    recorder_on_frame(ch);  // 10フレーム目で確定
    EXPECT_EQ(1u, recorder_ram_sample_count());

    for (int i = 0; i < 10; i++) recorder_on_frame(ch);
    EXPECT_EQ(2u, recorder_ram_sample_count());
}

// アナログchは窓末尾（最新）の値を採用
TEST(RecorderTest, AnalogChannelTakesLatestValue) {
    recorder_start();
    uint16_t ch[CRSF_NUM_CHANNELS];
    // ch0(スティック=アナログ)を毎フレーム変える。最後のフレームの値が残るはず。
    for (int f = 0; f < 10; f++) {
        make_channels(ch, 0, (uint16_t)(200 + f));
        recorder_on_frame(ch);
    }
    ASSERT_EQ(1u, recorder_ram_sample_count());

    uint16_t expected[CRSF_NUM_CHANNELS];
    make_channels(expected, 0, 209);  // 最終フレーム(f=9)の値
    expect_sample_equals(0, expected);
}

// スイッチchは窓内で一瞬でも押されたら最大値を保持（ラッチ）
TEST(RecorderTest, SwitchChannelLatchesBriefPress) {
    recorder_start();
    uint16_t ch[CRSF_NUM_CHANNELS];
    // ch6(=CH7 Arm, スイッチ扱い)を1フレームだけMAXにし、他はMIN
    for (int f = 0; f < 10; f++) {
        make_channels(ch, 6, (f == 3) ? CRSF_CHANNEL_MAX : CRSF_CHANNEL_MIN);
        recorder_on_frame(ch);
    }
    ASSERT_EQ(1u, recorder_ram_sample_count());

    uint16_t expected[CRSF_NUM_CHANNELS];
    // ch6は一瞬のMAXがラッチされMAXのまま、他はMID(=make_channelsの既定)。
    // ただし窓末尾フレームでch6=MIN, 他chはMID。ch6以外はMIDのまま。
    make_channels(expected, 6, CRSF_CHANNEL_MAX);
    expect_sample_equals(0, expected);
}

// stop_and_flush は端数の窓も末尾サンプルとして確定する
TEST(RecorderTest, FlushCommitsPartialWindow) {
    recorder_start();
    uint16_t ch[CRSF_NUM_CHANNELS];
    make_channels(ch, -1, 0);

    for (int i = 0; i < 25; i++) recorder_on_frame(ch);  // 2確定 + 端数5
    EXPECT_EQ(2u, recorder_ram_sample_count());

    size_t n = recorder_stop_and_flush();  // 端数を確定 → 3
    EXPECT_EQ(3u, n);
    EXPECT_EQ(3u, recorder_ram_sample_count());
}

// 停止後は on_frame を無視する
TEST(RecorderTest, IgnoresFramesWhenInactive) {
    recorder_init();
    uint16_t ch[CRSF_NUM_CHANNELS];
    make_channels(ch, -1, 0);
    for (int i = 0; i < 20; i++) recorder_on_frame(ch);
    EXPECT_EQ(0u, recorder_ram_sample_count());
}
