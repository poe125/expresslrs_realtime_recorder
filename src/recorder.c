#include "recorder.h"
#include <string.h>

// ========================================
// RAMバッファと記録状態
// ========================================

// 記録バッファ（パック済み payload を連結）。REC_MAX_SAMPLES に達したら停止。
static uint8_t  rec_buf[REC_MAX_SAMPLES * REC_SAMPLE_BYTES];
static uint32_t rec_count = 0;      // 確定済みサンプル数
static bool     rec_active = false;

// 間引き窓の累積。スイッチchは最大値ホールド、アナログchは最新値。
static uint16_t win_acc[CRSF_NUM_CHANNELS];
static uint16_t win_frames = 0;     // 現在の窓で累積したフレーム数

static inline bool is_switch_ch(int ch) {
    return (REC_SWITCH_CH_MASK >> ch) & 1u;
}

// 窓をリセット（スイッチchは押下検出のため下限にクリア。アナログchは
// 次フレームで最新値に上書きされるので値は問わない）。
static void window_reset(void) {
    win_frames = 0;
    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        win_acc[i] = is_switch_ch(i) ? CRSF_CHANNEL_MIN : CRSF_CHANNEL_MID;
    }
}

// 現在の窓累積を1サンプルとして確定・追記する。
static void window_commit(void) {
    if (rec_count < REC_MAX_SAMPLES) {
        crsf_pack_channels(win_acc, &rec_buf[rec_count * REC_SAMPLE_BYTES]);
        rec_count++;
    }
    window_reset();
}

void recorder_init(void) {
    rec_count = 0;
    rec_active = false;
    window_reset();
}

void recorder_start(void) {
    rec_count = 0;
    window_reset();
    rec_active = true;
}

void recorder_on_frame(const uint16_t channels[CRSF_NUM_CHANNELS]) {
    if (!rec_active) return;

    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        if (is_switch_ch(i)) {
            // 最大値ホールド（押下=MAX を窓内で保持）
            if (channels[i] > win_acc[i]) win_acc[i] = channels[i];
        } else {
            // アナログ: 窓末尾の最新値を採用
            win_acc[i] = channels[i];
        }
    }

    if (++win_frames >= REC_DECIMATION) {
        window_commit();
        if (rec_count >= REC_MAX_SAMPLES) {
            rec_active = false;  // バッファ満杯で自動停止
        }
    }
}

bool     recorder_is_active(void)        { return rec_active; }
bool     recorder_is_full(void)          { return rec_count >= REC_MAX_SAMPLES; }
uint32_t recorder_ram_sample_count(void) { return rec_count; }

const uint8_t* recorder_ram_sample(uint32_t idx) {
    if (idx >= rec_count) return NULL;
    return &rec_buf[idx * REC_SAMPLE_BYTES];
}

// ========================================
// フラッシュ書出し / 読出し
// ========================================

// フラッシュ内ログのヘッダ（先頭ページに配置）
typedef struct {
    uint32_t magic;         // REC_MAGIC
    uint16_t version;       // フォーマット版
    uint16_t rate_hz;       // 記録レート
    uint32_t sample_count;  // 有効サンプル数
    uint16_t sample_bytes;  // 1サンプルのバイト数
    uint16_t reserved;
} rec_header_t;

#define REC_MAGIC   0x31524C45u   // "ELR1"

#ifdef PICO_BOARD
#include "hardware/flash.h"
#include "hardware/sync.h"

// 2MBフラッシュの上位領域をログに割り当てる。
#define LOG_REGION_SIZE   (256u * 1024u)                     // 上位256KB
#define LOG_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - LOG_REGION_SIZE)

#define ALIGN_UP(x, a)    (((x) + (a) - 1u) & ~((a) - 1u))

static void flash_write_log(uint32_t count) {
    // 先頭ページにヘッダ
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    rec_header_t h = {
        .magic = REC_MAGIC, .version = 1, .rate_hz = REC_RATE_HZ,
        .sample_count = count, .sample_bytes = REC_SAMPLE_BYTES, .reserved = 0,
    };
    memcpy(page, &h, sizeof(h));

    uint32_t data_bytes = count * REC_SAMPLE_BYTES;
    uint32_t prog_bytes = ALIGN_UP(data_bytes, FLASH_PAGE_SIZE);      // program はページ単位
    uint32_t total      = FLASH_PAGE_SIZE + prog_bytes;
    uint32_t erase_bytes = ALIGN_UP(total, FLASH_SECTOR_SIZE);        // erase はセクタ単位
    if (erase_bytes > LOG_REGION_SIZE) erase_bytes = LOG_REGION_SIZE;

    // フラッシュ操作中はXIP停止・割込み禁止（500Hz送信は止まるが、
    // 記録停止=着地/待機中の想定なので許容）。
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(LOG_FLASH_OFFSET, erase_bytes);
    flash_range_program(LOG_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    if (prog_bytes > 0) {
        flash_range_program(LOG_FLASH_OFFSET + FLASH_PAGE_SIZE, rec_buf, prog_bytes);
    }
    restore_interrupts(ints);
}

uint32_t recorder_flash_sample_count(void) {
    const rec_header_t *h = (const rec_header_t *)(XIP_BASE + LOG_FLASH_OFFSET);
    if (h->magic != REC_MAGIC) return 0;
    if (h->sample_bytes != REC_SAMPLE_BYTES) return 0;
    if (h->sample_count > REC_MAX_SAMPLES) return 0;
    return h->sample_count;
}

const uint8_t* recorder_flash_sample(uint32_t idx) {
    if (idx >= recorder_flash_sample_count()) return NULL;
    return (const uint8_t *)(XIP_BASE + LOG_FLASH_OFFSET + FLASH_PAGE_SIZE
                             + (size_t)idx * REC_SAMPLE_BYTES);
}

#elif defined(BUILD_PI4)
// Raspberry Pi 4 (Linux) 用: フラッシュの代わりに通常のファイルへ保存する。
// 保存先は環境変数 RECORDER_LOG_PATH で上書き可能。既定は
// ~/.expresslrs_recorder/flight.log（Picoのファイル形式=ヘッダ+パック済み
// payload の連結、をそのままファイルに書く）。
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char* log_path(void) {
    static char path[512];
    const char *override = getenv("RECORDER_LOG_PATH");
    if (override && override[0] != '\0') {
        snprintf(path, sizeof(path), "%s", override);
        return path;
    }
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') home = "/tmp";
    char dir[480];
    snprintf(dir, sizeof(dir), "%s/.expresslrs_recorder", home);
    mkdir(dir, 0755);  // 既存でもOK。エラーは意図的に無視。
    snprintf(path, sizeof(path), "%s/flight.log", dir);
    return path;
}

// 読出し用キャッシュ（ファイル全体をRAMへロードしてXIP相当のポインタ参照を可能にする）
static uint8_t  flash_read_buf[REC_MAX_SAMPLES * REC_SAMPLE_BYTES];
static uint32_t flash_sample_count_cache = 0;
static bool     flash_loaded = false;

static void flash_write_log(uint32_t count) {
    FILE *f = fopen(log_path(), "wb");
    if (!f) {
        perror("recorder: failed to open log file for write");
        return;
    }
    rec_header_t h = {
        .magic = REC_MAGIC, .version = 1, .rate_hz = REC_RATE_HZ,
        .sample_count = count, .sample_bytes = REC_SAMPLE_BYTES, .reserved = 0,
    };
    fwrite(&h, sizeof(h), 1, f);
    if (count > 0) {
        fwrite(rec_buf, REC_SAMPLE_BYTES, count, f);
    }
    fclose(f);
    printf("# recording saved to %s (%u samples)\n", log_path(), (unsigned)count);
    flash_loaded = false;  // 直後のPLAYBACKで今回の書込みが読めるようキャッシュを破棄
}

static void flash_load_if_needed(void) {
    if (flash_loaded) return;
    flash_loaded = true;
    flash_sample_count_cache = 0;

    FILE *f = fopen(log_path(), "rb");
    if (!f) return;  // 記録がまだ無い

    rec_header_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 ||
        h.magic != REC_MAGIC ||
        h.sample_bytes != REC_SAMPLE_BYTES ||
        h.sample_count > REC_MAX_SAMPLES) {
        fclose(f);
        return;
    }
    flash_sample_count_cache = (uint32_t)fread(flash_read_buf, REC_SAMPLE_BYTES, h.sample_count, f);
    fclose(f);
}

uint32_t recorder_flash_sample_count(void) {
    flash_load_if_needed();
    return flash_sample_count_cache;
}

const uint8_t* recorder_flash_sample(uint32_t idx) {
    flash_load_if_needed();
    if (idx >= flash_sample_count_cache) return NULL;
    return &flash_read_buf[idx * REC_SAMPLE_BYTES];
}

#else
// ホスト（テスト）ではフラッシュ無し。書出しはno-op、読出しは空。
static void flash_write_log(uint32_t count) { (void)count; }
uint32_t recorder_flash_sample_count(void) { return 0; }
const uint8_t* recorder_flash_sample(uint32_t idx) { (void)idx; return NULL; }
#endif // PICO_BOARD / BUILD_PI4

size_t recorder_stop_and_flush(void) {
    rec_active = false;
    // 端数（未確定の窓）が1フレーム以上あれば末尾サンプルとして確定
    if (win_frames > 0) {
        window_commit();
    }
    flash_write_log(rec_count);
    return rec_count;
}
