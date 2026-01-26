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
    )
    target_include_directories(usb_gamepad_lib PUBLIC include)

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

    include(GoogleTest)
    gtest_discover_tests(test_crsf)
    gtest_discover_tests(test_usb_gamepad)

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
    )

    target_include_directories(expresslrs_realtime_recorder PRIVATE include)

    # Picoライブラリをリンク
    target_link_libraries(expresslrs_realtime_recorder
        pico_stdlib
        hardware_uart
        tinyusb_host
        tinyusb_board
    )

    # USB HostモードではUSB CDC stdioは使用不可
    # デバッグ出力はUARTを使用
    pico_enable_stdio_usb(expresslrs_realtime_recorder 0)
    pico_enable_stdio_uart(expresslrs_realtime_recorder 1)

    # UF2ファイル生成
    pico_add_extra_outputs(expresslrs_realtime_recorder)
endif()
