#pragma once

#include <cstdint>

namespace TraceRoutePolicy
{
inline uint32_t timeoutMs(bool mqttRoundTripEnabled)
{
    return mqttRoundTripEnabled ? 60000 : 10000;
}

enum class RoutingReport { Ignore, BrokerEcho, TargetAck, TargetError, LocalError };

inline RoutingReport routingReport(bool toUs, uint32_t from, uint32_t localNode, uint32_t requestId, bool tracking,
                                   uint32_t target, uint32_t activeRequestId, bool mqttTransport, bool hasError)
{
    if (!toUs || !tracking || !activeRequestId || requestId != activeRequestId || !target || target == UINT32_MAX)
        return RoutingReport::Ignore;
    if (from == target)
        return hasError ? RoutingReport::TargetError : RoutingReport::TargetAck;
    if (from == localNode)
        return hasError ? RoutingReport::LocalError : mqttTransport ? RoutingReport::BrokerEcho : RoutingReport::Ignore;
    return RoutingReport::Ignore;
}

inline bool matchesResponse(bool toUs, uint32_t from, uint32_t requestId, bool tracking, uint32_t target,
                            uint32_t activeRequestId)
{
    return toUs && tracking && target != 0 && target != UINT32_MAX && activeRequestId != 0 &&
           from == target && requestId == activeRequestId;
}

inline bool acceptsMqtt(bool toUs, bool wantResponse, uint32_t from, uint32_t requestId, bool tracking,
                        uint32_t target, uint32_t activeRequestId)
{
    if (!toUs || from == 0 || from == UINT32_MAX)
        return false;
    if (requestId == 0)
        return wantResponse;
    return matchesResponse(toUs, from, requestId, tracking, target, activeRequestId);
}
} // namespace TraceRoutePolicy
