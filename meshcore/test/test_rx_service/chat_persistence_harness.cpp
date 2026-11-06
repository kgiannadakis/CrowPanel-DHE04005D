#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <utility>
#include <string.h>
#define ESP32 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define FILE_APPEND "a"
using StaticSemaphore_t = int;
using SemaphoreHandle_t = int*;
static uint32_t now_ms = 1;
static int lock_depth, max_depth, failures;
static int render_depth;
static bool render_busy;
static bool lvgl_lock(int timeout_ms) {
    if (lock_depth || timeout_ms != 5) ++failures;
    if (render_busy) return false;
    ++render_depth;
    return true;
}
static void lvgl_unlock() { if (--render_depth < 0 || lock_depth) ++failures; }
static uint32_t millis() { return now_ms; }
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* p) { return p; }
static int xSemaphoreTakeRecursive(SemaphoreHandle_t, uint32_t) {
    if (++lock_depth > max_depth) max_depth = lock_depth;
    return pdTRUE;
}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t) { if (--lock_depth < 0) ++failures; }
enum class CrashStage { Storage };
static void crash_diagnostics_mark_current(CrashStage, unsigned) {}

extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
extern "C" void* memset(void* dst, int value, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(value);
    return dst;
}
extern "C" int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<uint8_t>(*a) - static_cast<uint8_t>(*b);
}
class String {
    char text_[768] = {};
    size_t size_ = 0;
public:
    String() = default;
    String(const char* text) { while (text[size_] && size_ < sizeof(text_) - 1) { text_[size_] = text[size_]; ++size_; } }
    size_t length() const { return size_; }
    const char* c_str() const { return text_; }
};
struct StrHelper {
    static void strncpy(char* dst, const char* src, size_t n) {
        size_t i = 0;
        for (; i + 1 < n && src[i]; ++i) dst[i] = src[i];
        dst[i] = 0;
    }
};
static String chat_path_for(const String& key) { return key; }
struct StoredFile { bool used; char name[20], text[4096]; size_t size; };
static StoredFile files[4];
static bool fail_open;
static int write_limit = -1, opens;
class File {
    StoredFile* file_;
public:
    explicit File(StoredFile* file = nullptr) : file_(file) {}
    explicit operator bool() const { return file_ != nullptr; }
    size_t write(const uint8_t* data, size_t n) {
        if (lock_depth <= 0 || render_depth <= 0 || n > 128) ++failures;
        if (write_limit >= 0 && n > static_cast<size_t>(write_limit)) { n = write_limit; write_limit = -1; }
        for (size_t i = 0; i < n; ++i) file_->text[file_->size++] = data[i];
        file_->text[file_->size] = 0;
        return n;
    }
    void close() { if (lock_depth <= 0 || render_depth <= 0) ++failures; }
};
struct MockFS {
    File open(const String& path, const char* mode) {
        if (lock_depth <= 0 || render_depth <= 0 || strcmp(mode, FILE_APPEND)) ++failures;
        ++opens;
        if (fail_open) return File();
        StoredFile* free_file = nullptr;
        for (auto& file : files) {
            if (file.used && !strcmp(file.name, path.c_str())) return File(&file);
            if (!file.used && !free_file) free_file = &file;
        }
        if (!free_file) { ++failures; return File(); }
        free_file->used = true;
        StrHelper::strncpy(free_file->name, path.c_str(), sizeof(free_file->name));
        return File(free_file);
    }
} LittleFS;

// INSERT_PRODUCTION_METHODS

static void reset() {
    for (auto& p : s_pending) { p.active = false; p.line = String(); p.written = 0; }
    for (auto& f : files) { f.used = false; f.size = 0; f.text[0] = 0; }
    s_pending_count = 0; g_chat_flush_deadline_ms = 0; s_chat_retry_after_ms = 0;
    lock_depth = max_depth = failures = opens = 0; fail_open = false; write_limit = -1; now_ms = 1;
    render_depth = 0; render_busy = false;
}
static bool contents(const char* name, const char* text) {
    for (auto& f : files) if (f.used && !strcmp(f.name, name)) return !strcmp(f.text, text);
    return false;
}
extern "C" uint32_t run_tests() {
    if (chat_append_flush_sync() || chat_enqueue_line("a", "x")) return 1;
    chat_persistence_init();
    reset();
    char long_line[401]; for (int i = 0; i < 400; ++i) long_line[i] = 'L'; long_line[400] = 0;
    chat_enqueue_line("a", long_line);
    if (!chat_append_flush_sync() || !contents("a", long_line) || s_pending_count || lock_depth || failures) return 2;
    reset();
    chat_enqueue_line("a", "A1\n"); chat_enqueue_line("b", "B1\n"); chat_enqueue_line("a", "A2\n");
    if (!chat_append_flush_sync() || !contents("a", "A1\nA2\n") || !contents("b", "B1\n") || opens != 2 || failures) return 3;
    reset(); fail_open = true; chat_enqueue_line("a", "saved\n");
    if (chat_append_flush_sync() || s_pending_count != 1 || lock_depth) return 4;
    const int previous_opens = opens;
    if (chat_append_flush_sync() || opens != previous_opens) return 5;
    fail_open = false; now_ms += 1001;
    if (!chat_append_flush_sync() || !contents("a", "saved\n") || failures) return 6;
    reset();
    chat_enqueue_line("a", "A1\n"); chat_enqueue_line("b", "B12345\n"); chat_enqueue_line("a", "A2\n");
    // Complete channel a, then fail partway through channel b.
    fail_open = true;
    if (chat_append_flush_sync()) return 7;
    fail_open = false; now_ms += 1001;
    // First a write is only 3 bytes, so the limit stays armed until b's larger write.
    write_limit = 3;
    if (chat_append_flush_sync() || s_pending_count != 1 || !contents("a", "A1\nA2\n") || !contents("b", "B12")) return 8;
    if (!chat_enqueue_line("b", "Bnext\n")) return 9;
    now_ms += 1001;
    if (!chat_append_flush_sync() || !contents("b", "B12345\nBnext\n") || failures || lock_depth) return 10;
    reset();
    for (int i = 0; i < 32; ++i) if (!chat_enqueue_line("a", "X")) return 11;
    if (!chat_enqueue_line("a", "Y") || s_pending_count != 1 || max_depth != 1 || lock_depth) return 12;
    if (!chat_append_flush_sync() || files[0].size != 33 || files[0].text[32] != 'Y' || failures) return 13;
    reset(); now_ms = 100; chat_enqueue_line("a", "due"); now_ms = 299; chat_append_flush_if_due();
    if (opens) return 14;
    now_ms = 300; chat_append_flush_if_due();
    if (!contents("a", "due") || g_chat_flush_deadline_ms.load() || failures || lock_depth) return 15;
    reset();
    chat_enqueue_line("a", "waiting\n"); render_busy = true;
    if (chat_append_flush_sync() || opens || s_pending_count != 1 || failures || lock_depth || render_depth) return 16;
    render_busy = false;
    if (!chat_append_flush_sync() || !contents("a", "waiting\n") || failures || render_depth) return 17;
    reset();
    for (int i = 0; i < 32; ++i) if (!chat_enqueue_line("a", "X")) return 18;
    render_busy = true;
    if (chat_enqueue_line("a", "Y") || s_pending_count != 32 || opens || failures || lock_depth || render_depth) return 19;
    render_busy = false;
    if (!chat_enqueue_line("a", "Y") || !chat_append_flush_sync() || files[0].size != 33 || failures || render_depth) return 20;
    return 0;
}
