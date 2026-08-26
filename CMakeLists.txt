cmake_minimum_required(VERSION 3.13)

# テストビルドかPicoビルドかを判定
option(BUILD_TESTS "Build unit tests for host" OFF)

if(BUILD_TESTS)
    # ======================================
    # ホスト向けテストビルド
    # ======================================
    project(expresslrs_realtime_recorder_tests CXX C)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # GoogleTestを取得
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    # CRSFライブラリ（テスト用、Pico非依存部分のみ）
    add_library(crsf_lib
        src/crsf.c
    )
    target_include_directories(crsf_lib PUBLIC include)

    # USB Gamepadライブラリ（テスト用）
    add_library(usb_gamepad_lib
        src/usb_gamepad.c
        src/hid_parser.c
    )
    target_include_directories(usb_gamepad_lib PUBLIC include)

    # Recorderライブラリ（テスト用、フラッシュ部はPico限定で除外される）
    add_library(recorder_lib
        src/recorder.c
    )
    target_include_directories(recorder_lib PUBLIC include)
    target_link_libraries(recorder_lib crsf_lib)

    # CRSFテスト
    add_executable(test_crsf
        tests/test_crsf.cpp
    )
    target_link_libraries(test_crsf
        crsf_lib
        GTest::gtest_main
    )
    target_include_directories(test_crsf PRIVATE include)

    # USB Gamepadテスト
    add_executable(test_usb_gamepad
        tests/test_usb_gamepad.cpp
    )
    target_link_libraries(test_usb_gamepad
        usb_gamepad_lib
        GTest::gtest_main
    )
    target_include_directories(test_usb_gamepad PRIVATE include)

    # HIDディスクリプタ解析テスト（未知のゲームパッド対応の要）
    add_executable(test_hid_parser
        tests/test_hid_parser.cpp
    )
    target_link_libraries(test_hid_parser
        usb_gamepad_lib
        GTest::gtest_main
    )
    target_include_directories(test_hid_parser PRIVATE include)

    # Recorderテスト（間引き・ボタンラッチのロジック検証）
    add_executable(test_recorder
        tests/test_recorder.cpp
    )
    target_link_libraries(test_recorder
        recorder_lib
        GTest::gtest_main
    )
    target_include_directories(test_recorder PRIVATE include)

    # gtest_discover_tests は各実行ファイルをビルド時に起動して列挙するが、
    # サンドボックス環境ではその起動がタイムアウトする。実行はテスト時のみに
    # したいので add_test で登録する（各バイナリを1テストとして実行）。
    add_test(NAME test_crsf        COMMAND test_crsf)
    add_test(NAME test_usb_gamepad COMMAND test_usb_gamepad)
    add_test(NAME test_hid_parser  COMMAND test_hid_parser)
    add_test(NAME test_recorder    COMMAND test_recorder)

else()
    # ======================================
    # Pico SDK向けビルド
    # ======================================

    # Pico SDKの初期化
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

    project(expresslrs_realtime_recorder C CXX ASM)

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    pico_sdk_init()

    # メインアプリケーション
    add_executable(expresslrs_realtime_recorder
        src/main.c
        src/crsf.c
        src/usb_gamepad.c
        src/hid_parser.c
        src/recorder.c
    )

    target_include_directories(expresslrs_realtime_recorder PRIVATE include)

    # Picoライブラリをリンク
    target_link_libraries(expresslrs_realtime_recorder
        pico_stdlib
        hardware_uart
        hardware_flash
        tinyusb_host
        tinyusb_board
    )

    # USB HostモードではUSB CDC stdioは使用不可
    # デバッグ出力はUARTを使用
    pico_enable_stdio_usb(expresslrs_realtime_recorder 0)
    pico_enable_stdio_uart(expresslrs_realtime_recorder 1)

    # stdio(printf/getchar) を uart1 = GP4(TX)/GP5(RX) にルーティング。
    # デフォルトの uart0(GP0) のままだと CRSF 出力線(GP0)に printf が混入し
    # CRSF 信号を破壊するため必須。
    # ボーレートは値確認ストリームの帯域確保のため 921600 に設定
    # （500Hz=2ms周期で1行を取りこぼさず出力するため）。
    target_compile_definitions(expresslrs_realtime_recorder PRIVATE
        PICO_DEFAULT_UART=1
        PICO_DEFAULT_UART_TX_PIN=4
        PICO_DEFAULT_UART_RX_PIN=5
        PICO_DEFAULT_UART_BAUD_RATE=921600
    )

    # UF2ファイル生成
    pico_add_extra_outputs(expresslrs_realtime_recorder)
endif()
