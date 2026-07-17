#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_MCU          OPT_MCU_RP2040
#define CFG_TUSB_OS           OPT_OS_NONE

// デバッグレベル（0:なし, 1:エラー, 2:警告, 3:情報）
// 列挙やエンドポイント転送を追いたい時は 2 にする。CMakeが -DCFG_TUSB_DEBUG=0 を
// 注入するため、#ifndef ガードでは上書きできず #undef が必要（Stage C調査で判明）。
#undef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0

// メモリアライメント
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

// USB Hostモードを有効化
#define CFG_TUH_ENABLED       1

// RP2040のUSBポートをHostモードで使用
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_HOST

// 同時接続可能なデバイス数
#define CFG_TUH_DEVICE_MAX    1

// 列挙バッファサイズ（HIDレポートディスクリプタ用）
#define CFG_TUH_ENUMERATION_BUFSIZE 256

//--------------------------------------------------------------------
// HID HOST CONFIGURATION
//--------------------------------------------------------------------

// HIDホストを有効化
#define CFG_TUH_HID           4

// HIDレポートバッファサイズ
#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

//--------------------------------------------------------------------
// HUB CONFIGURATION (オプション)
//--------------------------------------------------------------------

// USBハブサポート（必要に応じて有効化）
#define CFG_TUH_HUB           0

//--------------------------------------------------------------------
// OTHER CLASS DRIVERS (無効化)
//--------------------------------------------------------------------

#define CFG_TUH_CDC           0
#define CFG_TUH_MSC           0
#define CFG_TUH_VENDOR        0

#ifdef __cplusplus
}
#endif

#endif // TUSB_CONFIG_H
