#include <gtest/gtest.h>
#include <vector>

extern "C" {
#include "hid_parser.h"
}

// =============================================================================
// テスト用のレポートディスクリプタ
// =============================================================================

// 典型的なゲームパッド（F310 Dモード相当の並び）:
//   4軸 8bit (X, Y, Z, Rz) → ハット 4bit → パディング 4bit → ボタン12個
static std::vector<uint8_t> DescGamepad4Axis() {
    return {
        0x05, 0x01,        // Usage Page (Generic Desktop)
        0x09, 0x05,        // Usage (Game Pad)
        0xA1, 0x01,        // Collection (Application)
        0xA1, 0x00,        //   Collection (Physical)
        0x09, 0x30,        //     Usage (X)
        0x09, 0x31,        //     Usage (Y)
        0x09, 0x32,        //     Usage (Z)
        0x09, 0x35,        //     Usage (Rz)
        0x15, 0x00,        //     Logical Minimum (0)
        0x26, 0xFF, 0x00,  //     Logical Maximum (255)
        0x75, 0x08,        //     Report Size (8)
        0x95, 0x04,        //     Report Count (4)
        0x81, 0x02,        //     Input (Data,Var,Abs)
        0x09, 0x39,        //     Usage (Hat switch)
        0x15, 0x00,        //     Logical Minimum (0)
        0x25, 0x07,        //     Logical Maximum (7)
        0x75, 0x04,        //     Report Size (4)
        0x95, 0x01,        //     Report Count (1)
        0x81, 0x42,        //     Input (Data,Var,Abs,Null)
        0x75, 0x04,        //     Report Size (4)
        0x95, 0x01,        //     Report Count (1)
        0x81, 0x01,        //     Input (Const) ← パディング
        0x05, 0x09,        //     Usage Page (Button)
        0x19, 0x01,        //     Usage Minimum (1)
        0x29, 0x0C,        //     Usage Maximum (12)
        0x15, 0x00,        //     Logical Minimum (0)
        0x25, 0x01,        //     Logical Maximum (1)
        0x75, 0x01,        //     Report Size (1)
        0x95, 0x0C,        //     Report Count (12)
        0x81, 0x02,        //     Input (Data,Var,Abs)
        0xC0,              //   End Collection
        0xC0               // End Collection
    };
}

// ラジオ送信機に多い並び: 16bit 4軸を X, Y, Rx, Ry で表現し、レポートIDあり
static std::vector<uint8_t> DescRadio16bitRxRy() {
    return {
        0x05, 0x01,              // Usage Page (Generic Desktop)
        0x09, 0x04,              // Usage (Joystick)
        0xA1, 0x01,              // Collection (Application)
        0x85, 0x01,              //   Report ID (1)
        0x09, 0x30,              //   Usage (X)
        0x09, 0x31,              //   Usage (Y)
        0x09, 0x33,              //   Usage (Rx)
        0x09, 0x34,              //   Usage (Ry)
        0x16, 0x00, 0x00,        //   Logical Minimum (0)
        0x26, 0xFF, 0x07,        //   Logical Maximum (2047)
        0x75, 0x10,              //   Report Size (16)
        0x95, 0x04,              //   Report Count (4)
        0x81, 0x02,              //   Input (Data,Var,Abs)
        0xC0                     // End Collection
    };
}

// =============================================================================
// ディスクリプタ解析
// =============================================================================

TEST(HidParserTest, FindsFourAxesAndButtons) {
    auto desc = DescGamepad4Axis();
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc.data(), desc.size(), &l));

    EXPECT_EQ(l.report_id, 0);

    // X,Y,Z,Rz が 8bit ずつ先頭から並ぶ
    EXPECT_TRUE(l.axes[GAMEPAD_AXIS_LX].present);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LX].bit_offset, 0);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LX].bit_size, 8);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LY].bit_offset, 8);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_RX].bit_offset, 16);  // Z
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_RY].bit_offset, 24);  // Rz

    // ハットは4軸の直後
    EXPECT_TRUE(l.hat.present);
    EXPECT_EQ(l.hat.bit_offset, 32);
    EXPECT_EQ(l.hat.bit_size, 4);

    // ボタンはハット+パディングの後（byte5から）。
    // 宣言は12個だが、ビット11〜14はD-Pad用に予約されているため11個で打ち切る。
    EXPECT_EQ(l.button_count, 11);
    EXPECT_EQ(l.button_bit_offset, 40);
}

TEST(HidParserTest, MapsRxRyToRightStickWhenNoZRz) {
    // Z/Rz が無い機種では Rx/Ry を右スティックに割り当てる
    auto desc = DescRadio16bitRxRy();
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc.data(), desc.size(), &l));

    EXPECT_EQ(l.report_id, 1);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LX].bit_offset, 0);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LY].bit_offset, 16);
    EXPECT_TRUE(l.axes[GAMEPAD_AXIS_RX].present);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_RX].bit_offset, 32);  // Rx
    EXPECT_TRUE(l.axes[GAMEPAD_AXIS_RY].present);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_RY].bit_offset, 48);  // Ry
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LX].bit_size, 16);
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LX].logical_max, 2047);

    // 右スティックに使ったのでトリガーには割り当てない
    EXPECT_FALSE(l.axes[GAMEPAD_AXIS_L2].present);
    EXPECT_FALSE(l.axes[GAMEPAD_AXIS_R2].present);
}

TEST(HidParserTest, RejectsGarbageDescriptor) {
    // 軸が1つも無いディスクリプタは valid にならない（決め打ちパーサへ落ちる）
    uint8_t desc[] = { 0x05, 0x01, 0xA1, 0x01, 0xC0 };
    hid_layout_t l;
    EXPECT_FALSE(hid_parse_report_descriptor(desc, sizeof(desc), &l));
    EXPECT_FALSE(l.valid);
}

TEST(HidParserTest, DoesNotOverrunTruncatedDescriptor) {
    // 途中で切れたディスクリプタでも読み過ぎずに戻ること
    auto desc = DescGamepad4Axis();
    for (size_t n = 1; n < desc.size(); n++) {
        hid_layout_t l;
        hid_parse_report_descriptor(desc.data(), n, &l);  // クラッシュしなければ良い
    }
}

// =============================================================================
// レポートからの値取り出し
// =============================================================================

TEST(HidExtractTest, NormalizesCenterAndExtremes) {
    auto desc = DescGamepad4Axis();
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc.data(), desc.size(), &l));

    gamepad_state_t s = {};
    // X=0(最小), Y=128(中央), Z=255(最大), Rz=128, hat=8(中立), buttons=0
    uint8_t report[7] = { 0x00, 0x80, 0xFF, 0x80, 0x08, 0x00, 0x00 };
    ASSERT_TRUE(hid_extract_state(&l, report, sizeof(report), &s));

    EXPECT_EQ(s.axes[GAMEPAD_AXIS_LX], -32768);
    EXPECT_NEAR(s.axes[GAMEPAD_AXIS_LY], 0, 200);   // 128/255 はぴったり中央ではない
    EXPECT_EQ(s.axes[GAMEPAD_AXIS_RX], 32767);
    EXPECT_NEAR(s.axes[GAMEPAD_AXIS_RY], 0, 200);
    EXPECT_EQ(s.buttons, 0);
}

TEST(HidExtractTest, ReadsButtonsAndHat) {
    auto desc = DescGamepad4Axis();
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc.data(), desc.size(), &l));

    gamepad_state_t s = {};
    // hat=2(東) と ボタン1(bit0) と ボタン11(bit10) を立てる
    uint8_t report[7] = { 0x80, 0x80, 0x80, 0x80, 0x02, 0x01, 0x04 };
    ASSERT_TRUE(hid_extract_state(&l, report, sizeof(report), &s));

    EXPECT_TRUE(s.buttons & GAMEPAD_BTN_A);          // ボタン1 = bit0
    EXPECT_TRUE(s.buttons & (1 << 10));              // ボタン11 = 取り込む上限
    EXPECT_TRUE(s.buttons & GAMEPAD_BTN_DPAD_R);     // hat 東
    EXPECT_FALSE(s.buttons & GAMEPAD_BTN_DPAD_U);
}

TEST(HidExtractTest, ButtonsDoNotClobberDpadBits) {
    // ボタン12（bit11 = DPAD_U と同じビット）は取り込まないこと。
    // 取り込むとハット中立なのに「上」を押した扱いになる。
    auto desc = DescGamepad4Axis();
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc.data(), desc.size(), &l));

    gamepad_state_t s = {};
    // hat=8(中立), ボタン12(bit11)だけ押下
    uint8_t report[7] = { 0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0x08 };
    ASSERT_TRUE(hid_extract_state(&l, report, sizeof(report), &s));

    EXPECT_FALSE(s.buttons & GAMEPAD_BTN_DPAD_U);
    EXPECT_EQ(s.buttons, 0);
}

TEST(HidExtractTest, HonorsReportId) {
    auto desc = DescRadio16bitRxRy();
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc.data(), desc.size(), &l));

    gamepad_state_t s = {};
    // レポートID=1。以降 16bit LE で X=0, Y=1023(中央), Rx=2047, Ry=1023
    uint8_t good[9] = { 0x01, 0x00, 0x00, 0xFF, 0x03, 0xFF, 0x07, 0xFF, 0x03 };
    ASSERT_TRUE(hid_extract_state(&l, good, sizeof(good), &s));
    EXPECT_EQ(s.axes[GAMEPAD_AXIS_LX], -32768);
    EXPECT_NEAR(s.axes[GAMEPAD_AXIS_LY], 0, 100);
    EXPECT_EQ(s.axes[GAMEPAD_AXIS_RX], 32767);

    // 別のレポートIDは受け付けない
    uint8_t other[9] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    EXPECT_FALSE(hid_extract_state(&l, other, sizeof(other), &s));
}

TEST(HidExtractTest, SurvivesShortReport) {
    // 宣言より短いレポートが来ても読み過ぎない
    auto desc = DescGamepad4Axis();
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc.data(), desc.size(), &l));

    gamepad_state_t s = {};
    uint8_t report[2] = { 0xFF, 0x00 };
    EXPECT_TRUE(hid_extract_state(&l, report, sizeof(report), &s));
    EXPECT_EQ(s.axes[GAMEPAD_AXIS_LX], 32767);
}

TEST(HidExtractTest, SignedAxisIsSignExtended) {
    // 論理範囲が -127..127 の機種（符号付き8bit軸）
    uint8_t desc[] = {
        0x05, 0x01,        // Usage Page (Generic Desktop)
        0x09, 0x04,        // Usage (Joystick)
        0xA1, 0x01,        // Collection (Application)
        0x09, 0x30,        //   Usage (X)
        0x09, 0x31,        //   Usage (Y)
        0x15, 0x81,        //   Logical Minimum (-127)
        0x25, 0x7F,        //   Logical Maximum (127)
        0x75, 0x08,        //   Report Size (8)
        0x95, 0x02,        //   Report Count (2)
        0x81, 0x02,        //   Input (Data,Var,Abs)
        0xC0               // End Collection
    };
    hid_layout_t l;
    ASSERT_TRUE(hid_parse_report_descriptor(desc, sizeof(desc), &l));
    EXPECT_EQ(l.axes[GAMEPAD_AXIS_LX].logical_min, -127);

    gamepad_state_t s = {};
    uint8_t report[2] = { 0x81, 0x00 };  // -127, 0
    ASSERT_TRUE(hid_extract_state(&l, report, sizeof(report), &s));
    EXPECT_EQ(s.axes[GAMEPAD_AXIS_LX], -32768);
    EXPECT_NEAR(s.axes[GAMEPAD_AXIS_LY], 0, 200);
}
