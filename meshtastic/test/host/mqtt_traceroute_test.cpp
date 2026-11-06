#include "modules/TraceRoutePolicy.h"
#include "mqtt/MqttTextPolicy.h"

#include <cassert>
#include <cstdio>

int main()
{
    constexpr uint32_t target = 0xacaae48c;
    constexpr uint32_t packetId = 0x455190bf;
    constexpr uint32_t localNode = 0x0ae01942;
    // The channel's broker round trip enables the long deadline, even for a
    // saved contact incorrectly labelled as RF-only.
    assert(TraceRoutePolicy::timeoutMs(true) == 60000);
    assert(TraceRoutePolicy::timeoutMs(false) == 10000);
    using TraceRoutePolicy::RoutingReport;
    using TraceRoutePolicy::routingReport;
    assert(routingReport(true, localNode, localNode, packetId, true, target, packetId, true, false) ==
           RoutingReport::BrokerEcho);
    assert(routingReport(true, target, localNode, packetId, true, target, packetId, false, false) ==
           RoutingReport::TargetAck);
    assert(routingReport(true, target, localNode, packetId, true, target, packetId, true, false) ==
           RoutingReport::TargetAck);
    assert(routingReport(true, target, localNode, packetId, true, target, packetId, true, true) ==
           RoutingReport::TargetError);
    assert(routingReport(true, localNode, localNode, packetId, true, target, packetId, false, true) ==
           RoutingReport::LocalError);
    assert(routingReport(true, localNode, localNode, packetId, true, target, packetId, false, false) ==
           RoutingReport::Ignore);
    assert(routingReport(false, target, localNode, packetId, true, target, packetId, true, false) ==
           RoutingReport::Ignore);
    assert(routingReport(true, target + 1, localNode, packetId, true, target, packetId, true, false) ==
           RoutingReport::Ignore);
    assert(routingReport(true, target, localNode, packetId + 1, true, target, packetId, true, false) ==
           RoutingReport::Ignore);
    assert(routingReport(true, target, localNode, packetId, false, target, packetId, true, false) ==
           RoutingReport::Ignore);
    using TraceRoutePolicy::acceptsMqtt;
    using TraceRoutePolicy::matchesResponse;
    assert(acceptsMqtt(true, false, target, packetId, true, target, packetId));
    assert(matchesResponse(true, target, packetId, true, target, packetId));
    assert(!acceptsMqtt(false, false, target, packetId, true, target, packetId));
    assert(!acceptsMqtt(true, false, target + 1, packetId, true, target, packetId));
    assert(!acceptsMqtt(true, false, target, packetId + 1, true, target, packetId));
    assert(!acceptsMqtt(true, false, target, packetId, false, target, packetId));
    assert(!acceptsMqtt(true, false, target, packetId, true, 0, packetId));
    assert(!acceptsMqtt(true, false, target, packetId, true, target, 0));
    assert(!acceptsMqtt(true, false, 0, packetId, true, target, packetId));
    assert(!acceptsMqtt(true, false, UINT32_MAX, packetId, true, UINT32_MAX, packetId));

    // Requests to us work independently of local trace state and contact discovery.
    assert(acceptsMqtt(true, true, target, 0, false, 0, 0));
    assert(!acceptsMqtt(true, false, target, 0, false, 0, 0));
    assert(!acceptsMqtt(false, true, target, 0, false, 0, 0));
    assert(!acceptsMqtt(true, true, target, packetId + 1, true, target, packetId));
    assert(MqttTextPolicy::accepts(meshtastic_PortNum_TRACEROUTE_APP, 0, true, false, false, true));
    assert(MqttTextPolicy::accepts(meshtastic_PortNum_TRACEROUTE_APP, 10, true, true, false, true));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_TRACEROUTE_APP, 10, true, true, true));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_TRACEROUTE_APP, 10, false, true, true, true));

    for (int port = 0; port < 512; ++port) {
        const auto type = static_cast<meshtastic_PortNum>(port);
        const bool essential = type == meshtastic_PortNum_TEXT_MESSAGE_APP || type == meshtastic_PortNum_ROUTING_APP ||
                               type == meshtastic_PortNum_TRACEROUTE_APP;
        assert(MqttTextPolicy::accepts(type, 10, true, true, false, true) == essential);
    }
    for (uint32_t id = 1; id < 100000; ++id)
        assert(!acceptsMqtt(true, true, target, id, true, target, packetId));
    std::puts("PASS: MQTT trace deadlines, broker echo vs target ACK/NAK, active ID/target/destination correlation, port filter");
}
