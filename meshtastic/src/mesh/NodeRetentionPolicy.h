#pragma once

#include <cstddef>
#include <cstdint>

namespace NodeRetentionPolicy
{
inline bool mqttOnlyAfterPacket(bool mqttOnly, bool packetViaMqtt)
{
    // Once heard on RF, a node remains protected from MQTT cache replacement.
    return mqttOnly && packetViaMqtt;
}

template <typename Node>
int oldestMqttContact(const Node *nodes, size_t count, uint32_t localNode, uint32_t verifiedMask)
{
    int oldest = -1;
    for (size_t i = 0; i < count; ++i) {
        const auto &node = nodes[i];
        if (!node.num || node.num == localNode || !node.via_mqtt || node.is_favorite || node.is_ignored ||
            (node.bitfield & verifiedMask))
            continue;
        if (oldest < 0 || node.last_heard < nodes[oldest].last_heard)
            oldest = static_cast<int>(i);
    }
    return oldest;
}
} // namespace NodeRetentionPolicy
