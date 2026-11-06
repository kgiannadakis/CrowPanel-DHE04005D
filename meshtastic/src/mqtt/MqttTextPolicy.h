#pragma once

#include "mesh/generated/meshtastic/portnums.pb.h"
#include <cstddef>

namespace MqttTextPolicy
{
inline bool accepts(meshtastic_PortNum port, size_t payloadSize, bool toUs, bool hasRequestId, bool contactDiscoveryEnabled,
                    bool allowTraceroute = false)
{
    if (port == meshtastic_PortNum_TEXT_MESSAGE_APP)
        return payloadSize != 0;
    // Names, keys and map coordinates are contact metadata, not chat messages.
    if (port == meshtastic_PortNum_NODEINFO_APP || port == meshtastic_PortNum_POSITION_APP)
        return contactDiscoveryEnabled && payloadSize != 0;
    // Empty RouteDiscovery requests are valid; only the trace module may opt in.
    if (port == meshtastic_PortNum_TRACEROUTE_APP)
        return toUs && allowTraceroute;
    return toUs && port == meshtastic_PortNum_ROUTING_APP && hasRequestId;
}
} // namespace MqttTextPolicy
