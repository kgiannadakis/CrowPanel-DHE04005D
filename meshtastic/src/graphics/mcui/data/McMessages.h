#pragma once
#if HAS_TFT && USE_MCUI

#include <cstdint>
#include <cstddef>

namespace mcui {

struct McConvId {
    enum Kind : uint8_t { CHANNEL = 0, DIRECT = 1, INVALID = 0xFF };
    Kind kind;

    uint32_t value;

    bool operator==(const McConvId &o) const { return kind == o.kind && value == o.value; }
    bool is_valid() const { return kind != INVALID; }
    static McConvId none() { return {INVALID, 0}; }
    static McConvId channel(uint8_t ch) { return {CHANNEL, ch}; }
    static McConvId direct(uint32_t node) { return {DIRECT, node}; }
};

struct McMessage {
    uint32_t from_node;
    uint32_t timestamp;
    float    snr;
    int16_t  rssi;
    bool     outgoing;
    bool     delivered;

    bool     ack_failed;

    uint32_t packet_id;

    char     text[220];

};

constexpr int MC_MAX_CONVERSATIONS = 8;

// 16 (was 32): the store is heap_caps_malloc(MALLOC_CAP_INTERNAL) on this
// board; halving it frees ~31 KB internal RAM and halves the save-time
// serialize buffer, widening the headroom before internal exhaustion spills
// onto the corrupted post-framebuffer PSRAM heap.
constexpr int MC_MAX_MSGS_PER_CONV = 16;

void messages_init();

void messages_append(const McConvId &id, const McMessage &msg);

void messages_attach_packet_id(const McConvId &id, uint32_t packet_id);

void messages_mark_delivered_by_packet_id(uint32_t packet_id);

void messages_mark_ack_by_packet_id(uint32_t packet_id, bool success);

void messages_mark_last_unsent_as_failed(const McConvId &id);

size_t messages_snapshot(const McConvId &id, McMessage *out, size_t max_count);

bool messages_last(const McConvId &id, McMessage &out);

uint16_t messages_unread(const McConvId &id);

void messages_mark_read(const McConvId &id);

bool messages_delete_conv(const McConvId &id);

typedef void (*McConvVisitor)(const McConvId &id, void *ctx);
void messages_for_each_conv(McConvVisitor visit, void *ctx);

uint32_t messages_change_tick();

void messages_load();
void messages_save();

void messages_save_tick();

void node_rssi_set(uint32_t node_num, int16_t rssi);
int16_t node_rssi_get(uint32_t node_num);

}

#endif
