#include "mesh/NodeRetentionPolicy.h"

#include <array>
#include <cassert>
#include <cstdio>

struct Node {
    uint32_t num = 0;
    uint32_t last_heard = 0;
    uint32_t bitfield = 0;
    bool via_mqtt = false;
    bool is_favorite = false;
    bool is_ignored = false;
};

int main()
{
    constexpr uint32_t ours = 1;
    constexpr uint32_t verifiedMask = 1;
    std::array<Node, 300> nodes{};
    for (size_t i = 0; i < nodes.size(); ++i)
        nodes[i] = {static_cast<uint32_t>(i + 1), static_cast<uint32_t>(1000 + i), 0, true, false, false};
    nodes[0].last_heard = 0; // Self can never be replaced, even if incorrectly flagged MQTT.
    nodes[1].last_heard = 0;
    nodes[1].is_favorite = true;
    nodes[2].last_heard = 0;
    nodes[2].is_ignored = true;
    nodes[3].last_heard = 0;
    nodes[3].bitfield = verifiedMask;
    nodes[4].last_heard = 0;
    nodes[4].via_mqtt = false; // RF or legacy unknown origin is protected.
    assert(NodeRetentionPolicy::oldestMqttContact(nodes.data(), nodes.size(), ours, verifiedMask) == 5);
    nodes[10].last_heard = 1;
    assert(NodeRetentionPolicy::oldestMqttContact(nodes.data(), nodes.size(), ours, verifiedMask) == 10);
    nodes[10].last_heard = 5000; // Refreshing a node makes it newer, not an eviction candidate.
    assert(NodeRetentionPolicy::oldestMqttContact(nodes.data(), nodes.size(), ours, verifiedMask) == 5);
    nodes[6].last_heard = 0; // Unknown last-heard time is older than a known timestamp.
    assert(NodeRetentionPolicy::oldestMqttContact(nodes.data(), nodes.size(), ours, verifiedMask) == 6);
    for (uint32_t id = 2000; id < 12000; ++id) {
        const int slot = NodeRetentionPolicy::oldestMqttContact(nodes.data(), nodes.size(), ours, verifiedMask);
        assert(slot >= 5 && slot < static_cast<int>(nodes.size()));
        nodes[slot] = {id, id + 10000, 0, true, false, false};
    }
    assert(nodes[0].num == ours && nodes[1].is_favorite && nodes[2].is_ignored && nodes[3].bitfield == verifiedMask);
    assert(nodes[4].num == 5 && !nodes[4].via_mqtt);
    for (auto &node : nodes) node.via_mqtt = false;
    assert(NodeRetentionPolicy::oldestMqttContact(nodes.data(), nodes.size(), ours, verifiedMask) == -1);
    assert(NodeRetentionPolicy::oldestMqttContact(nodes.data(), 0, ours, verifiedMask) == -1);
    assert(NodeRetentionPolicy::mqttOnlyAfterPacket(true, true));
    assert(!NodeRetentionPolicy::mqttOnlyAfterPacket(true, false));
    assert(!NodeRetentionPolicy::mqttOnlyAfterPacket(false, true));
    assert(!NodeRetentionPolicy::mqttOnlyAfterPacket(false, false));
    std::puts("PASS: 300-slot MQTT LRU, protected/self/RF contacts, refresh, 10000 replacements, sticky RF provenance");
}
