#include <gtest/gtest.h>

extern "C" {
#include "usb_gamepad.h"

// テスト用関数（usb_gamepad.c で定義）
void usb_gamepad_test_set_state(const gamepad_state_t *state);
}

// =============================================================================
// 軸変換テスト
// =============================================================================

TEST(GamepadAxisTest, CenterValue) {
    // 128 (中央) → 0
    EXPECT_EQ(gamepad_axis_u8_to_s16(128), 0);
}

TEST(GamepadAxisTest, MinValue) {
    // 0 → -32768
    EXPECT_EQ(gamepad_axis_u8_to_s16(0), -32768);
}

TEST(GamepadAxisTest, MaxValue) {
    // 255 → 32512 (255-128)*256 = 32512
    // 注: 完全な32767にはならない（8bit→16bit変換の制限）
    int16_t result = gamepad_axis_u8_to_s16(255);
    EXPECT_GT(result, 32000);
    EXPECT_LE(result, 32767);
}

TEST(GamepadAxisTest, QuarterNegative) {
    // 64 (25%位置) → 負の値
    int16_t result = gamepad_axis_u8_to_s16(64);
    EXPECT_LT(result, 0);
    EXPECT_GT(result, -32768);
}

TEST(GamepadAxisTest, QuarterPositive) {
    // 192 (75%位置) → 正の値
    int16_t result = gamepad_axis_u8_to_s16(192);
    EXPECT_GT(result, 0);
    EXPECT_LT(result, 32767);
}

// =============================================================================
// D-Pad変換テスト
// =============================================================================

TEST(GamepadDpadTest, Neutral) {
    // 8 = neutral → ボタンなし
    EXPECT_EQ(gamepad_dpad_to_buttons(8), 0);
}

TEST(GamepadDpadTest, North) {
    // 0 = N → Up
    EXPECT_EQ(gamepad_dpad_to_buttons(0), GAMEPAD_BTN_DPAD_U);
}

TEST(GamepadDpadTest, East) {
    // 2 = E → Right
    EXPECT_EQ(gamepad_dpad_to_buttons(2), GAMEPAD_BTN_DPAD_R);
}

TEST(GamepadDpadTest, South) {
    // 4 = S → Down
    EXPECT_EQ(gamepad_dpad_to_buttons(4), GAMEPAD_BTN_DPAD_D);
}

TEST(GamepadDpadTest, West) {
    // 6 = W → Left
    EXPECT_EQ(gamepad_dpad_to_buttons(6), GAMEPAD_BTN_DPAD_L);
}

TEST(GamepadDpadTest, NorthEast) {
    // 1 = NE → Up + Right
    uint16_t expected = GAMEPAD_BTN_DPAD_U | GAMEPAD_BTN_DPAD_R;
    EXPECT_EQ(gamepad_dpad_to_buttons(1), expected);
}

TEST(GamepadDpadTest, SouthWest) {
    // 5 = SW → Down + Left
    uint16_t expected = GAMEPAD_BTN_DPAD_D | GAMEPAD_BTN_DPAD_L;
    EXPECT_EQ(gamepad_dpad_to_buttons(5), expected);
}

TEST(GamepadDpadTest, Invalid) {
    // 無効な値 → ボタンなし
    EXPECT_EQ(gamepad_dpad_to_buttons(9), 0);
    EXPECT_EQ(gamepad_dpad_to_buttons(255), 0);
}

// =============================================================================
// ゲームパッド状態テスト
// =============================================================================

TEST(GamepadStateTest, InitialState) {
    usb_gamepad_init();

    const gamepad_state_t *state = usb_gamepad_get_state();

    EXPECT_FALSE(state->connected);
    EXPECT_EQ(state->buttons, 0);
    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) {
        EXPECT_EQ(state->axes[i], 0);
    }
}

TEST(GamepadStateTest, SetState) {
    usb_gamepad_init();

    gamepad_state_t test_state = {0};
    test_state.connected = true;
    test_state.axes[GAMEPAD_AXIS_LX] = 1000;
    test_state.axes[GAMEPAD_AXIS_LY] = -1000;
    test_state.buttons = GAMEPAD_BTN_A | GAMEPAD_BTN_B;
    test_state.vid = 0x054C;
    test_state.pid = 0x09CC;

    usb_gamepad_test_set_state(&test_state);

    const gamepad_state_t *state = usb_gamepad_get_state();

    EXPECT_TRUE(state->connected);
    EXPECT_EQ(state->axes[GAMEPAD_AXIS_LX], 1000);
    EXPECT_EQ(state->axes[GAMEPAD_AXIS_LY], -1000);
    EXPECT_EQ(state->buttons, GAMEPAD_BTN_A | GAMEPAD_BTN_B);
    EXPECT_EQ(state->vid, 0x054C);
    EXPECT_EQ(state->pid, 0x09CC);
}

// =============================================================================
// コールバックテスト
// =============================================================================

static gamepad_state_t callback_received_state;
static int callback_count = 0;

static void test_callback(const gamepad_state_t *state) {
    callback_received_state = *state;
    callback_count++;
}

TEST(GamepadCallbackTest, CallbackInvoked) {
    usb_gamepad_init();

    callback_count = 0;
    usb_gamepad_set_callback(test_callback);

    gamepad_state_t test_state = {0};
    test_state.connected = true;
    test_state.axes[GAMEPAD_AXIS_LX] = 5000;

    usb_gamepad_test_set_state(&test_state);

    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(callback_received_state.axes[GAMEPAD_AXIS_LX], 5000);

    // クリーンアップ
    usb_gamepad_set_callback(NULL);
}

// =============================================================================
// 接続状態テスト
// =============================================================================

TEST(GamepadConnectionTest, NotConnected) {
    usb_gamepad_init();
    EXPECT_FALSE(usb_gamepad_is_connected());
}

TEST(GamepadConnectionTest, Connected) {
    usb_gamepad_init();

    gamepad_state_t test_state = {0};
    test_state.connected = true;

    usb_gamepad_test_set_state(&test_state);

    EXPECT_TRUE(usb_gamepad_is_connected());
}

// =============================================================================
// メイン
// =============================================================================

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
