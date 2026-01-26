#include <stdio.h>
#include "crsf.h"

// Pico SDK headers (Picoビルド時のみ有効)
#ifdef PICO_BOARD
#include "pico/stdlib.h"
#include "hardware/uart.h"
#endif

int main() {
#ifdef PICO_BOARD
    stdio_init_all();

    printf("ExpressLRS Realtime Recorder\n");
    printf("Starting...\n");

    // TODO: USB Host初期化
    // TODO: UART初期化（CRSF用、420000bps）
    // TODO: メインループ実装

    while (1) {
        // メインループ（未実装）
        sleep_ms(1000);
    }
#else
    printf("This is a Pico application. Build with Pico SDK.\n");
#endif

    return 0;
}
