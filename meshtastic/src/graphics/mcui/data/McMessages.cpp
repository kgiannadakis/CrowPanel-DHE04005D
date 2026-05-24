#if HAS_TFT && USE_MCUI

#include "McMessages.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "configuration.h"

#include <cstring>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mcui {

static volatile bool s_dirty = false;
static uint32_t s_last_save_ms = 0;
static constexpr uint32_t PERSIST_MIN_INTERVAL_MS = 2000;
static const char *PERSIST_PATH = "/prefs/mcui_msgs.bin";
static constexpr uint32_t PERSIST_MAGIC = 0x4D43554D;

static constexpr uint16_t PERSIST_VERSION = 3;

struct ConvEntry {
    McConvId id;
    McMessage ring[MC_MAX_MSGS_PER_CONV];
    uint8_t head;
    uint8_t count;
    uint16_t unread;
};

static ConvEntry *s_convs = nullptr;
static int s_num_convs = 0;
static SemaphoreHandle_t s_lock = nullptr;
static volatile uint32_t s_change_tick = 0;

static ConvEntry *find_or_create_locked(const McConvId &id)
{
    for (int i = 0; i < s_num_convs; i++) {
        if (s_convs[i].id == id) return &s_convs[i];
    }
    if (s_num_convs >= MC_MAX_CONVERSATIONS) {

        int evict = -1;
        for (int i = 0; i < s_num_convs; i++) {
            if (s_convs[i].id.kind == McConvId::DIRECT) {
                if (evict < 0) evict = i;
                else if (s_convs[i].ring[(s_convs[i].head + MC_MAX_MSGS_PER_CONV - 1) %
                                        MC_MAX_MSGS_PER_CONV].timestamp <
                         s_convs[evict].ring[(s_convs[evict].head + MC_MAX_MSGS_PER_CONV - 1) %
                                             MC_MAX_MSGS_PER_CONV].timestamp) {
                    evict = i;
                }
            }
        }
        if (evict < 0) return nullptr;

        for (int i = evict; i < s_num_convs - 1; i++) s_convs[i] = s_convs[i + 1];
        s_num_convs--;
    }
    ConvEntry *c = &s_convs[s_num_convs++];
    memset(c, 0, sizeof(*c));
    c->id = id;
    return c;
}

void messages_init()
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_convs) {
        size_t bytes = sizeof(ConvEntry) * MC_MAX_CONVERSATIONS;

        s_convs = (ConvEntry *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_convs) {
            s_convs = (ConvEntry *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
        }
        if (!s_convs) {
            LOG_ERROR("mcui: failed to allocate %u bytes for message store",
                      (unsigned)bytes);
            return;
        }
        memset(s_convs, 0, bytes);
    }
    s_num_convs = 0;
    s_change_tick = 0;

    messages_load();
    s_dirty = false;
    s_last_save_ms = 0;
}

void messages_append(const McConvId &id, const McMessage &msg)
{
    if (!s_lock || !s_convs) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ConvEntry *c = find_or_create_locked(id);
    if (c) {
        c->ring[c->head] = msg;
        c->head = (c->head + 1) % MC_MAX_MSGS_PER_CONV;
        if (c->count < MC_MAX_MSGS_PER_CONV) c->count++;
        if (!msg.outgoing) c->unread++;
    }
    s_change_tick++;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

size_t messages_snapshot(const McConvId &id, McMessage *out, size_t max_count)
{
    if (!s_lock || !s_convs || !out || max_count == 0) return 0;
    size_t copied = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        if (!(s_convs[i].id == id)) continue;
        ConvEntry *c = &s_convs[i];
        size_t n = c->count;
        if (n > max_count) n = max_count;

        int start = (c->head + MC_MAX_MSGS_PER_CONV - n) % MC_MAX_MSGS_PER_CONV;
        for (size_t k = 0; k < n; k++) {
            out[k] = c->ring[(start + k) % MC_MAX_MSGS_PER_CONV];
        }
        copied = n;
        break;
    }
    xSemaphoreGive(s_lock);
    return copied;
}

bool messages_last(const McConvId &id, McMessage &out)
{
    if (!s_lock || !s_convs) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        if (!(s_convs[i].id == id)) continue;
        ConvEntry *c = &s_convs[i];
        if (c->count > 0) {
            int idx = (c->head + MC_MAX_MSGS_PER_CONV - 1) % MC_MAX_MSGS_PER_CONV;
            out = c->ring[idx];
            ok = true;
        }
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void messages_attach_packet_id(const McConvId &id, uint32_t packet_id)
{
    if (!s_lock || !s_convs || packet_id == 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        if (!(s_convs[i].id == id)) continue;
        ConvEntry *c = &s_convs[i];

        for (int k = (int)c->count - 1; k >= 0; k--) {
            int idx = (c->head + MC_MAX_MSGS_PER_CONV - 1 - (c->count - 1 - k)) % MC_MAX_MSGS_PER_CONV;
            McMessage &m = c->ring[idx];
            if (m.outgoing && m.packet_id == 0) {
                m.packet_id = packet_id;
                break;
            }
        }
        break;
    }
    xSemaphoreGive(s_lock);
}

void messages_mark_delivered_by_packet_id(uint32_t packet_id)
{
    if (!s_lock || !s_convs || packet_id == 0) return;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs && !changed; i++) {
        ConvEntry *c = &s_convs[i];
        for (uint8_t k = 0; k < c->count; k++) {
            McMessage &m = c->ring[k];
            if (m.outgoing && m.packet_id == packet_id && !m.delivered) {
                m.delivered = true;
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        s_change_tick++;
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

void messages_mark_ack_by_packet_id(uint32_t packet_id, bool success)
{
    if (!s_lock || !s_convs || packet_id == 0) return;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs && !changed; i++) {
        ConvEntry *c = &s_convs[i];
        for (uint8_t k = 0; k < c->count; k++) {
            McMessage &m = c->ring[k];
            if (m.outgoing && m.packet_id == packet_id && !m.delivered) {
                m.delivered = true;
                m.ack_failed = !success;
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        s_change_tick++;
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

void messages_mark_last_unsent_as_failed(const McConvId &id)
{
    if (!s_lock || !s_convs) return;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        if (!(s_convs[i].id == id)) continue;
        ConvEntry *c = &s_convs[i];

        for (int k = (int)c->count - 1; k >= 0; k--) {
            int idx = (c->head + MC_MAX_MSGS_PER_CONV - 1 - (c->count - 1 - k)) % MC_MAX_MSGS_PER_CONV;
            McMessage &m = c->ring[idx];
            if (m.outgoing && m.packet_id == 0 && !m.delivered) {
                m.delivered  = true;
                m.ack_failed = true;
                changed = true;
                break;
            }
        }
        break;
    }
    if (changed) {
        s_change_tick++;
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

uint16_t messages_unread(const McConvId &id)
{
    if (!s_lock || !s_convs) return 0;
    uint16_t u = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        if (s_convs[i].id == id) { u = s_convs[i].unread; break; }
    }
    xSemaphoreGive(s_lock);
    return u;
}

void messages_mark_read(const McConvId &id)
{
    if (!s_lock || !s_convs) return;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        if (s_convs[i].id == id) {

            if (s_convs[i].unread != 0) {
                s_convs[i].unread = 0;
                changed = true;
            }
            break;
        }
    }
    if (changed) {
        s_change_tick++;

        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

bool messages_delete_conv(const McConvId &id)
{
    if (!s_lock || !s_convs || !id.is_valid()) return false;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        if (!(s_convs[i].id == id)) continue;
        for (int j = i; j < s_num_convs - 1; j++) {
            s_convs[j] = s_convs[j + 1];
        }
        if (s_num_convs > 0) {
            memset(&s_convs[s_num_convs - 1], 0, sizeof(ConvEntry));
            s_num_convs--;
        }
        changed = true;
        break;
    }
    if (changed) {
        s_change_tick++;
        s_dirty = true;
        s_last_save_ms = 0;
    }
    xSemaphoreGive(s_lock);
    return changed;
}

void messages_for_each_conv(McConvVisitor visit, void *ctx)
{
    if (!s_lock || !s_convs || !visit) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_convs; i++) {
        visit(s_convs[i].id, ctx);
    }
    xSemaphoreGive(s_lock);
}

uint32_t messages_change_tick()
{
    return s_change_tick;
}

#ifdef FSCom
static void ensure_parent_dir(const char *path)
{

    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) return;
    char dir[64];
    size_t n = (size_t)(last_slash - path);
    if (n >= sizeof(dir)) n = sizeof(dir) - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    FSCom.mkdir(dir);
}
#endif

void messages_save()
{
#ifdef FSCom
    if (!s_lock || !s_convs) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t total = 4  + 2  + 2 ;
    for (int i = 0; i < s_num_convs; i++) {
        ConvEntry *c = &s_convs[i];
        total += 1 + 4 + 2 + 2;
        total += (size_t)c->count * sizeof(McMessage);
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        xSemaphoreGive(s_lock);
        LOG_ERROR("mcui: messages_save: OOM (%u bytes)", (unsigned)total);
        return;
    }

    uint8_t *p = buf;
    auto put_u32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
    auto put_u16 = [&](uint16_t v) { memcpy(p, &v, 2); p += 2; };
    auto put_u8  = [&](uint8_t v)  { *p++ = v; };

    put_u32(PERSIST_MAGIC);
    put_u16(PERSIST_VERSION);
    put_u16((uint16_t)s_num_convs);
    for (int i = 0; i < s_num_convs; i++) {
        ConvEntry *c = &s_convs[i];
        put_u8((uint8_t)c->id.kind);
        put_u32(c->id.value);
        put_u16((uint16_t)c->count);
        put_u16(c->unread);

        int start = (c->head + MC_MAX_MSGS_PER_CONV - c->count) % MC_MAX_MSGS_PER_CONV;
        for (uint8_t k = 0; k < c->count; k++) {
            const McMessage &m = c->ring[(start + k) % MC_MAX_MSGS_PER_CONV];
            memcpy(p, &m, sizeof(McMessage));
            p += sizeof(McMessage);
        }
    }

    const uint32_t snapshot_tick = s_change_tick;
    xSemaphoreGive(s_lock);

    concurrency::LockGuard g(spiLock);
    ensure_parent_dir(PERSIST_PATH);

    const char *tmp = "/prefs/mcui_msgs.tmp";
    FSCom.remove(tmp);
    auto f = FSCom.open(tmp, FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("mcui: messages_save: cannot open %s for write", tmp);
        free(buf);

        return;
    }
    size_t written = f.write(buf, total);
    f.close();
    free(buf);

    if (written != total) {
        LOG_ERROR("mcui: messages_save: short write %u/%u", (unsigned)written, (unsigned)total);
        FSCom.remove(tmp);

        return;
    }

    FSCom.remove(PERSIST_PATH);
    FSCom.rename(tmp, PERSIST_PATH);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_change_tick == snapshot_tick) {
        s_dirty = false;
    }
    xSemaphoreGive(s_lock);

    LOG_INFO("mcui: saved %u bytes of chat history", (unsigned)total);
#endif
}

void messages_load()
{
#ifdef FSCom
    if (!s_lock || !s_convs) return;

    uint8_t *buf = nullptr;
    size_t total = 0;
    {
        concurrency::LockGuard g(spiLock);
        if (!FSCom.exists(PERSIST_PATH)) return;
        auto f = FSCom.open(PERSIST_PATH, FILE_O_READ);
        if (!f) return;
        total = f.size();
        if (total < 8 || total > 300000) {
            LOG_WARN("mcui: messages_load: bad size %u", (unsigned)total);
            f.close();
            return;
        }
        buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) {
            LOG_ERROR("mcui: messages_load: OOM (%u bytes)", (unsigned)total);
            f.close();
            return;
        }
        size_t got = f.readBytes((char *)buf, total);
        f.close();
        if (got != total) {
            LOG_WARN("mcui: messages_load: short read %u/%u", (unsigned)got, (unsigned)total);
            free(buf);
            return;
        }
    }

    uint8_t *p = buf;
    uint8_t *end = buf + total;
    auto need = [&](size_t n) { return (size_t)(end - p) >= n; };

    if (!need(8)) { free(buf); return; }
    uint32_t magic; memcpy(&magic, p, 4); p += 4;
    uint16_t ver;   memcpy(&ver,   p, 2); p += 2;
    uint16_t nconv; memcpy(&nconv, p, 2); p += 2;
    if (magic != PERSIST_MAGIC || ver != PERSIST_VERSION) {
        LOG_WARN("mcui: messages_load: magic/version mismatch %08x/%u", (unsigned)magic, (unsigned)ver);
        free(buf);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_num_convs = 0;
    for (uint16_t i = 0; i < nconv && s_num_convs < MC_MAX_CONVERSATIONS; i++) {
        if (!need(1 + 4 + 2 + 2)) break;
        uint8_t kind = *p++;
        uint32_t value; memcpy(&value, p, 4); p += 4;
        uint16_t count; memcpy(&count, p, 2); p += 2;
        uint16_t unread; memcpy(&unread, p, 2); p += 2;
        if (count > MC_MAX_MSGS_PER_CONV) { count = 0; break; }
        if (!need((size_t)count * sizeof(McMessage))) break;

        ConvEntry *c = &s_convs[s_num_convs++];
        memset(c, 0, sizeof(*c));
        c->id.kind = (McConvId::Kind)kind;
        c->id.value = value;
        c->unread = unread;
        for (uint16_t k = 0; k < count; k++) {
            memcpy(&c->ring[k], p, sizeof(McMessage));
            p += sizeof(McMessage);
        }
        c->count = (uint8_t)count;
        c->head = (uint8_t)(count % MC_MAX_MSGS_PER_CONV);
    }
    s_change_tick++;
    s_dirty = false;
    xSemaphoreGive(s_lock);
    free(buf);

    LOG_INFO("mcui: loaded %u conversations from flash", (unsigned)s_num_convs);
#endif
}

void messages_save_tick()
{
    if (!s_dirty) return;
    uint32_t now = millis();
    if (s_last_save_ms != 0 && (now - s_last_save_ms) < PERSIST_MIN_INTERVAL_MS) return;
    s_last_save_ms = now;
    messages_save();
}

struct RssiEntry {
    uint32_t node_num;
    int16_t rssi;
    uint32_t stamp;
};
static constexpr int RSSI_CACHE_SIZE = 32;
static RssiEntry s_rssi[RSSI_CACHE_SIZE] = {};
static uint32_t s_rssi_stamp = 0;

void node_rssi_set(uint32_t node_num, int16_t rssi)
{
    if (node_num == 0 || rssi == 0) return;
    s_rssi_stamp++;

    for (int i = 0; i < RSSI_CACHE_SIZE; i++) {
        if (s_rssi[i].node_num == node_num) {
            s_rssi[i].rssi = rssi;
            s_rssi[i].stamp = s_rssi_stamp;
            return;
        }
    }

    int evict = 0;
    for (int i = 0; i < RSSI_CACHE_SIZE; i++) {
        if (s_rssi[i].node_num == 0) { evict = i; break; }
        if (s_rssi[i].stamp < s_rssi[evict].stamp) evict = i;
    }
    s_rssi[evict].node_num = node_num;
    s_rssi[evict].rssi = rssi;
    s_rssi[evict].stamp = s_rssi_stamp;
}

int16_t node_rssi_get(uint32_t node_num)
{
    if (node_num == 0) return 0;
    for (int i = 0; i < RSSI_CACHE_SIZE; i++) {
        if (s_rssi[i].node_num == node_num) return s_rssi[i].rssi;
    }
    return 0;
}

}

#endif
