#include "TraceRouteModule.h"
#include "TraceRoutePolicy.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "mesh/Router.h"
#include "mesh/Channels.h"
#include "mesh/Throttle.h"
#if !MESHTASTIC_EXCLUDE_MQTT
#include "mqtt/MQTT.h"
#endif
#include "meshUtils.h"
#include <vector>

extern graphics::Screen *screen;

TraceRouteModule *traceRouteModule;

void TraceRouteModule::setResultText(const String &text)
{
    resultText = text;
    resultLines.clear();
    resultLinesDirty = true;
}

void TraceRouteModule::clearResultLines()
{
    resultLines.clear();
    resultLinesDirty = false;
}
#if HAS_SCREEN
void TraceRouteModule::rebuildResultLines(OLEDDisplay *display)
{
    if (!display) {
        resultLinesDirty = false;
        return;
    }

    resultLines.clear();

    if (resultText.length() == 0) {
        resultLinesDirty = false;
        return;
    }

    int maxWidth = display->getWidth() - 4;
    if (maxWidth <= 0) {
        resultLinesDirty = false;
        return;
    }

    int start = 0;
    int textLength = resultText.length();

    while (start <= textLength) {
        int newlinePos = resultText.indexOf('\n', start);
        String segment;

        if (newlinePos != -1) {
            segment = resultText.substring(start, newlinePos);
            start = newlinePos + 1;
        } else {
            segment = resultText.substring(start);
            start = textLength + 1;
        }

        if (segment.length() == 0) {
            resultLines.push_back("");
            continue;
        }

        if (display->getStringWidth(segment) <= maxWidth) {
            resultLines.push_back(segment);
            continue;
        }

        String remaining = segment;

        while (remaining.length() > 0) {
            String tempLine = "";
            int lastGoodBreak = -1;
            bool lineComplete = false;

            for (int i = 0; i < static_cast<int>(remaining.length()); i++) {
                char ch = remaining.charAt(i);
                String testLine = tempLine + ch;

                if (display->getStringWidth(testLine) > maxWidth) {
                    if (lastGoodBreak >= 0) {
                        resultLines.push_back(remaining.substring(0, lastGoodBreak + 1));
                        remaining = remaining.substring(lastGoodBreak + 1);
                        lineComplete = true;
                        break;
                    } else if (tempLine.length() > 0) {
                        resultLines.push_back(tempLine);
                        remaining = remaining.substring(i);
                        lineComplete = true;
                        break;
                    } else {
                        resultLines.push_back(String(ch));
                        remaining = remaining.substring(i + 1);
                        lineComplete = true;
                        break;
                    }
                } else {
                    tempLine = testLine;
                    if (ch == ' ' || ch == '>' || ch == '<' || ch == '-' || ch == '(' || ch == ')' || ch == ',') {
                        lastGoodBreak = i;
                    }
                }
            }

            if (!lineComplete) {
                if (tempLine.length() > 0) {
                    resultLines.push_back(tempLine);
                }
                break;
            }
        }
    }

    resultLinesDirty = false;
}
#endif

bool TraceRouteModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_RouteDiscovery *r)
{
    // We only alter the packet in alterReceivedProtobuf()
    return false; // let it be handled by RoutingModule
}

void TraceRouteModule::alterReceivedProtobuf(meshtastic_MeshPacket &p, meshtastic_RouteDiscovery *r)
{
    const meshtastic_Data &incoming = p.decoded;

    // Update next-hops using returned route
    if (incoming.request_id && !p.via_mqtt) {
        updateNextHops(p, r);
    }

    // Insert unknown hops if necessary
    insertUnknownHops(p, r, !incoming.request_id);

    // Append ID and SNR. If the last hop is to us, we only need to append the SNR
    // Broker delivery has no RF SNR, and must not teach the LoRa router a next hop.
    const float snr = p.via_mqtt ? static_cast<float>(INT8_MIN) / 4.0f : p.rx_snr;
    appendMyIDandSNR(r, snr, !incoming.request_id, isToUs(&p));
    if (!incoming.request_id)
        printRoute(r, p.from, p.to, true);
    else
        printRoute(r, p.to, p.from, false);

    // Set updated route to the payload of the to be flooded packet
    p.decoded.payload.size =
        pb_encode_to_bytes(p.decoded.payload.bytes, sizeof(p.decoded.payload.bytes), &meshtastic_RouteDiscovery_msg, r);

    if (tracingNode != 0) {
        // check isResponseFromTarget
        bool isResponseFromTarget = (incoming.request_id != 0 && p.from == tracingNode);
        bool isRequestToUs = (incoming.request_id == 0 && p.to == nodeDB->getNodeNum() && tracingNode != 0);

        // Check if this is a trace route response containing our target node
        bool containsTargetNode = false;
        for (uint8_t i = 0; i < r->route_count; i++) {
            if (r->route[i] == tracingNode) {
                containsTargetNode = true;
                break;
            }
        }
        for (uint8_t i = 0; i < r->route_back_count; i++) {
            if (r->route_back[i] == tracingNode) {
                containsTargetNode = true;
                break;
            }
        }

        // Check if this response contains a complete route to our target
        bool hasCompleteRoute = (r->route_count > 0 && r->route_back_count > 0) ||
                                (containsTargetNode && (r->route_count > 0 || r->route_back_count > 0));

        LOG_INFO("TracRoute packet analysis: tracingNode=0x%08x, p.from=0x%08x, p.to=0x%08x, request_id=0x%08x", tracingNode,
                 p.from, p.to, incoming.request_id);
        LOG_INFO("TracRoute conditions: isResponseFromTarget=%d, isRequestToUs=%d, containsTargetNode=%d, hasCompleteRoute=%d",
                 isResponseFromTarget, isRequestToUs, containsTargetNode, hasCompleteRoute);

        if (TraceRoutePolicy::matchesResponse(isToUs(&p), p.from, incoming.request_id,
                                             runState == TRACEROUTE_STATE_TRACKING, tracingNode, tracingPacketId)) {
            LOG_INFO("TracRoute result detected: isResponseFromTarget=%d, isRequestToUs=%d", isResponseFromTarget, isRequestToUs);

            LOG_INFO("SNR arrays - towards_count=%d, back_count=%d", r->snr_towards_count, r->snr_back_count);
            for (int i = 0; i < r->snr_towards_count; i++) {
                LOG_INFO("SNR towards[%d] = %d (%.1fdB)", i, r->snr_towards[i], (float)r->snr_towards[i] / 4.0f);
            }
            for (int i = 0; i < r->snr_back_count; i++) {
                LOG_INFO("SNR back[%d] = %d (%.1fdB)", i, r->snr_back[i], (float)r->snr_back[i] / 4.0f);
            }

            String result = p.via_mqtt ? "MQTT: " : "";

            // Show request path (from initiator to target)
            if (r->route_count > 0) {
                result += getNodeName(nodeDB->getNodeNum());
                for (uint8_t i = 0; i < r->route_count; i++) {
                    result += " > ";
                    const char *name = getNodeName(r->route[i]);
                    float snr =
                        (i < r->snr_towards_count && r->snr_towards[i] != INT8_MIN) ? ((float)r->snr_towards[i] / 4.0f) : 0.0f;
                    result += name;
                    if (snr != 0.0f) {
                        result += "(";
                        result += String(snr, 1);
                        result += "dB)";
                    }
                }
                result += " > ";
                result += getNodeName(tracingNode);
                if (r->snr_towards_count > 0 && r->snr_towards[r->snr_towards_count - 1] != INT8_MIN) {
                    result += "(";
                    result += String((float)r->snr_towards[r->snr_towards_count - 1] / 4.0f, 1);
                    result += "dB)";
                }
                result += "\n";
            } else {
                // Direct connection (no intermediate hops)
                result += getNodeName(nodeDB->getNodeNum());
                result += " > ";
                result += getNodeName(tracingNode);
                if (r->snr_towards_count > 0 && r->snr_towards[0] != INT8_MIN) {
                    result += "(";
                    result += String((float)r->snr_towards[0] / 4.0f, 1);
                    result += "dB)";
                }
                result += "\n";
            }

            // Show response path (from target back to initiator)
            if (p.via_mqtt)
                result += "MQTT: ";
            if (r->route_back_count > 0) {
                result += getNodeName(tracingNode);
                for (int8_t i = r->route_back_count - 1; i >= 0; i--) {
                    result += " > ";
                    const char *name = getNodeName(r->route_back[i]);
                    float snr = (i < r->snr_back_count && r->snr_back[i] != INT8_MIN) ? ((float)r->snr_back[i] / 4.0f) : 0.0f;
                    result += name;
                    if (snr != 0.0f) {
                        result += "(";
                        result += String(snr, 1);
                        result += "dB)";
                    }
                }
                // add initiator node
                result += " > ";
                result += getNodeName(nodeDB->getNodeNum());
                if (r->snr_back_count > 0 && r->snr_back[r->snr_back_count - 1] != INT8_MIN) {
                    result += "(";
                    result += String((float)r->snr_back[r->snr_back_count - 1] / 4.0f, 1);
                    result += "dB)";
                }
            } else {
                // Direct return path (no intermediate hops)
                result += getNodeName(tracingNode);
                result += " > ";
                result += getNodeName(nodeDB->getNodeNum());
                if (r->snr_back_count > 0 && r->snr_back[0] != INT8_MIN) {
                    result += "(";
                    result += String((float)r->snr_back[0] / 4.0f, 1);
                    result += "dB)";
                }
            }

            LOG_INFO("Trace route result: %s", result.c_str());
            handleTraceRouteResult(result);
        }
    }
}

void TraceRouteModule::updateNextHops(const meshtastic_MeshPacket &p, meshtastic_RouteDiscovery *r)
{
    // E.g. if the route is A->B->C->D and we are B, we can set C as next-hop for C and D
    // Similarly, if we are C, we can set D as next-hop for D
    // If we are A, we can set B as next-hop for B, C and D

    // First check if we were the original sender or in the original route
    int8_t nextHopIndex = -1;
    if (isToUs(&p)) {
        nextHopIndex = 0; // We are the original sender, next hop is first in route
    } else {
        // Check if we are in the original route
        for (uint8_t i = 0; i < r->route_count; i++) {
            if (r->route[i] == nodeDB->getNodeNum()) {
                nextHopIndex = i + 1; // Next hop is the one after us
                break;
            }
        }
    }

    // If we are in the original route, update the next hops
    if (nextHopIndex != -1) {
        // For every node after us, we can set the next-hop to the first node after us
        NodeNum nextHop;
        if (nextHopIndex == r->route_count) {
            nextHop = p.from; // We are the last in the route, next hop is destination
        } else {
            nextHop = r->route[nextHopIndex];
        }

        if (nextHop == NODENUM_BROADCAST) {
            return;
        }
        uint8_t nextHopByte = nodeDB->getLastByteOfNodeNum(nextHop);

        // For the rest of the nodes in the route, set their next-hop
        // Note: if we are the last in the route, this loop will not run
        for (int8_t i = nextHopIndex; i < r->route_count; i++) {
            NodeNum targetNode = r->route[i];
            maybeSetNextHop(targetNode, nextHopByte);
        }

        // Also set next-hop for the destination node
        maybeSetNextHop(p.from, nextHopByte);
    }
}

void TraceRouteModule::maybeSetNextHop(NodeNum target, uint8_t nextHopByte)
{
    if (target == NODENUM_BROADCAST)
        return;

    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(target);
    if (node && node->next_hop != nextHopByte) {
        LOG_INFO("Updating next-hop for 0x%08x to 0x%02x based on traceroute", target, nextHopByte);
        node->next_hop = nextHopByte;
    }
}

void TraceRouteModule::processUpgradedPacket(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag || mp.decoded.portnum != meshtastic_PortNum_TRACEROUTE_APP)
        return;

    meshtastic_RouteDiscovery decoded = meshtastic_RouteDiscovery_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_RouteDiscovery_msg, &decoded))
        return;

    handleReceivedProtobuf(mp, &decoded);
    // Intentionally modify the packet in-place so downstream relays see our updates.
    alterReceivedProtobuf(const_cast<meshtastic_MeshPacket &>(mp), &decoded);
}

void TraceRouteModule::insertUnknownHops(meshtastic_MeshPacket &p, meshtastic_RouteDiscovery *r, bool isTowardsDestination)
{
    pb_size_t *route_count;
    uint32_t *route;
    pb_size_t *snr_count;
    int8_t *snr_list;

    // Pick the correct route array and SNR list
    if (isTowardsDestination) {
        route_count = &r->route_count;
        route = r->route;
        snr_count = &r->snr_towards_count;
        snr_list = r->snr_towards;
    } else {
        route_count = &r->route_back_count;
        route = r->route_back;
        snr_count = &r->snr_back_count;
        snr_list = r->snr_back;
    }

    // Only insert unknown hops if hop_start is valid
    const int8_t hopsTaken = getHopsAway(p);
    if (hopsTaken >= 0) {
        int8_t diff = hopsTaken - *route_count;
        for (int8_t i = 0; i < diff; i++) {
            if (*route_count < ROUTE_SIZE) {
                route[*route_count] = NODENUM_BROADCAST; // This will represent an unknown hop
                *route_count += 1;
            }
        }
        // Add unknown SNR values if necessary
        diff = *route_count - *snr_count;
        for (int8_t i = 0; i < diff; i++) {
            if (*snr_count < ROUTE_SIZE) {
                snr_list[*snr_count] = INT8_MIN; // This will represent an unknown SNR
                *snr_count += 1;
            }
        }
    }
}

void TraceRouteModule::appendMyIDandSNR(meshtastic_RouteDiscovery *updated, float snr, bool isTowardsDestination, bool SNRonly)
{
    pb_size_t *route_count;
    uint32_t *route;
    pb_size_t *snr_count;
    int8_t *snr_list;

    // Pick the correct route array and SNR list
    if (isTowardsDestination) {
        route_count = &updated->route_count;
        route = updated->route;
        snr_count = &updated->snr_towards_count;
        snr_list = updated->snr_towards;
    } else {
        route_count = &updated->route_back_count;
        route = updated->route_back;
        snr_count = &updated->snr_back_count;
        snr_list = updated->snr_back;
    }

    if (*snr_count < ROUTE_SIZE) {
        snr_list[*snr_count] = (int8_t)(snr * 4); // Convert SNR to 1 byte
        *snr_count += 1;
    }
    if (SNRonly)
        return;

    // Length of route array can normally not be exceeded due to the max. hop_limit of 7
    if (*route_count < ROUTE_SIZE) {
        route[*route_count] = myNodeInfo.my_node_num;
        *route_count += 1;
    } else {
        LOG_WARN("Route exceeded maximum hop limit!"); // Are you bridging networks?
    }
}

void TraceRouteModule::printRoute(meshtastic_RouteDiscovery *r, uint32_t origin, uint32_t dest, bool isTowardsDestination)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    std::string route = "Route traced:\n";
    route += vformat("0x%x --> ", origin);
    for (uint8_t i = 0; i < r->route_count; i++) {
        if (i < r->snr_towards_count && r->snr_towards[i] != INT8_MIN)
            route += vformat("0x%x (%.2fdB) --> ", r->route[i], (float)r->snr_towards[i] / 4);
        else
            route += vformat("0x%x (?dB) --> ", r->route[i]);
    }
    // If we are the destination, or it has already reached the destination, print it
    if (dest == nodeDB->getNodeNum() || !isTowardsDestination) {
        if (r->snr_towards_count > 0 && r->snr_towards[r->snr_towards_count - 1] != INT8_MIN)
            route += vformat("0x%x (%.2fdB)", dest, (float)r->snr_towards[r->snr_towards_count - 1] / 4);

        else
            route += vformat("0x%x (?dB)", dest);
    } else
        route += "...";

    // If there's a route back (or we are the destination as then the route is complete), print it
    if (r->route_back_count > 0 || origin == nodeDB->getNodeNum()) {
        route += "\n";
        if (r->snr_back_count > 0 && origin == nodeDB->getNodeNum() && r->snr_back[r->snr_back_count - 1] != INT8_MIN)
            route += vformat("(%.2fdB) 0x%x <-- ", (float)r->snr_back[r->snr_back_count - 1] / 4, origin);
        else
            route += "...";

        for (int8_t i = r->route_back_count - 1; i >= 0; i--) {
            if (i < r->snr_back_count && r->snr_back[i] != INT8_MIN)
                route += vformat("(%.2fdB) 0x%x <-- ", (float)r->snr_back[i] / 4, r->route_back[i]);
            else
                route += vformat("(?dB) 0x%x <-- ", r->route_back[i]);
        }
        route += vformat("0x%x", dest);
    }
    LOG_INFO(route.c_str());
#endif
}

meshtastic_MeshPacket *TraceRouteModule::allocReply()
{
    assert(currentRequest);

    // Ignore multi-hop broadcast requests
    if (isBroadcast(currentRequest->to) && currentRequest->hop_limit < currentRequest->hop_start) {
        ignoreRequest = true;
        return NULL;
    }

    // Copy the payload of the current request
    auto req = *currentRequest;
    const auto &p = req.decoded;
    meshtastic_RouteDiscovery scratch;
    meshtastic_RouteDiscovery *updated = NULL;
    memset(&scratch, 0, sizeof(scratch));
    if (!pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_RouteDiscovery_msg, &scratch)) {
        ignoreRequest = true;
        return nullptr;
    }
    updated = &scratch;

    // Create a MeshPacket with this payload and set it as the reply
    meshtastic_MeshPacket *reply = allocDataProtobuf(*updated);

    return reply;
}

TraceRouteModule::TraceRouteModule()
    : ProtobufModule("traceroute", meshtastic_PortNum_TRACEROUTE_APP, &meshtastic_RouteDiscovery_msg), OSThread("TraceRoute")
{
    ourPortNum = meshtastic_PortNum_TRACEROUTE_APP;
    isPromiscuous = true; // We need to update the route even if it is not destined to us
}

const char *TraceRouteModule::getNodeName(NodeNum node)
{
    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(node);
    if (info && info->has_user) {
        if (strlen(info->user.short_name) > 0) {
            return info->user.short_name;
        }
        if (strlen(info->user.long_name) > 0) {
            return info->user.long_name;
        }
    }

    static char fallback[12];
    snprintf(fallback, sizeof(fallback), "0x%08x", node);
    return fallback;
}

bool TraceRouteModule::startTraceRoute(NodeNum node)
{
    LOG_INFO("=== TraceRoute startTraceRoute CALLED: node=0x%08x ===", node);
    unsigned long now = millis();
    if (!nodeDB || !router || !service) {
        handleTraceRouteResult("Router/service unavailable");
        return false;
    }

    if (node == 0 || node == NODENUM_BROADCAST) {
        LOG_ERROR("Invalid node number for trace route: 0x%08x", node);
        runState = TRACEROUTE_STATE_RESULT;
        setResultText("Invalid node");
        resultShowTime = millis();
        tracingNode = 0;

        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        return false;
    }

    if (node == nodeDB->getNodeNum()) {
        LOG_ERROR("Cannot trace route to self: 0x%08x", node);
        runState = TRACEROUTE_STATE_RESULT;
        setResultText("Cannot trace self");
        resultShowTime = millis();
        tracingNode = 0;

        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        return false;
    }

    if (!initialized) {
        lastTraceRouteTime = 0;
        initialized = true;
        LOG_INFO("TraceRoute initialized for first time");
    }

    if (runState == TRACEROUTE_STATE_TRACKING) {
        LOG_INFO("TraceRoute already in progress");
        return false;
    }

    if (initialized && lastTraceRouteTime > 0 && Throttle::isWithinTimespanMs(lastTraceRouteTime, cooldownMs)) {
        // Cooldown
        unsigned long wait = (cooldownMs - (now - lastTraceRouteTime) + 999) / 1000;
        bannerText = String("Wait for ") + String(wait) + String("s");
        runState = TRACEROUTE_STATE_COOLDOWN;
        resultText = "";
        clearResultLines();

        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        LOG_INFO("Cooldown active, please wait %lu seconds before starting a new trace route.", wait);
        return false;
    }

    tracingNode = node;
    lastTraceRouteTime = now;
    runState = TRACEROUTE_STATE_TRACKING;
    resultText = "";
    clearResultLines();
    bannerText = String("Tracing ") + getNodeName(node);

    LOG_INFO("TraceRoute UI: Starting trace route to node 0x%08x, requesting focus", node);

    // 请求焦点，然后触发UI更新事件
    requestFocus();
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);

    // 设置定时器来处理超时检查
    setIntervalFromNow(1000); // 每秒检查一次状态

    meshtastic_RouteDiscovery req = meshtastic_RouteDiscovery_init_zero;
    LOG_INFO("Creating RouteDiscovery protobuf...");

    // Allocate a packet directly from router like the reference code
    meshtastic_MeshPacket *p = router->allocForSending();
    if (p) {
        if (!prepareRequest(p, node)) {
            packetPool.release(p);
            return false;
        }
        // Set destination and port
        p->to = node;
        p->decoded.portnum = meshtastic_PortNum_TRACEROUTE_APP;
        p->decoded.want_response = true;

        // Use reliable delivery for traceroute requests (which will be copied to traceroute responses by setReplyTo)
        p->want_ack = true;

        // Manually encode the RouteDiscovery payload
        p->decoded.payload.size =
            pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_RouteDiscovery_msg, &req);

        LOG_INFO("Packet allocated successfully: to=0x%08x, portnum=%d, want_response=%d, payload_size=%d", p->to,
                 p->decoded.portnum, p->decoded.want_response, p->decoded.payload.size);
        LOG_INFO("About to call service->sendToMesh...");

        if (service) {
            LOG_INFO("MeshService is available, sending packet...");
            service->sendToMesh(p, RX_SRC_USER);
            LOG_INFO("sendToMesh called successfully for trace route to node 0x%08x", node);
        } else {
            LOG_ERROR("MeshService is NULL!");
            packetPool.release(p);
            runState = TRACEROUTE_STATE_RESULT;
            setResultText("Service unavailable");
            resultShowTime = millis();
            tracingNode = 0;

            requestFocus();
            UIFrameEvent e2;
            e2.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e2);
            return false;
        }
    } else {
        LOG_ERROR("Failed to allocate TraceRoute packet from router");
        runState = TRACEROUTE_STATE_RESULT;
        setResultText("Failed to send");
        resultShowTime = millis();
        tracingNode = 0;

        requestFocus();
        UIFrameEvent e2;
        e2.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e2);
        return false;
    }
    return true;
}

void TraceRouteModule::launch(NodeNum node)
{
    if (!nodeDB || !router || !service) {
        handleTraceRouteResult("Router/service unavailable");
        return;
    }
    if (runState == TRACEROUTE_STATE_TRACKING)
        return;
    if (node == 0 || node == NODENUM_BROADCAST) {
        LOG_ERROR("Invalid node number for trace route: 0x%08x", node);
        runState = TRACEROUTE_STATE_RESULT;
        setResultText("Invalid node");
        resultShowTime = millis();
        tracingNode = 0;

        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        return;
    }

    if (node == nodeDB->getNodeNum()) {
        LOG_ERROR("Cannot trace route to self: 0x%08x", node);
        runState = TRACEROUTE_STATE_RESULT;
        setResultText("Cannot trace self");
        resultShowTime = millis();
        tracingNode = 0;

        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        return;
    }

    if (!initialized) {
        lastTraceRouteTime = 0;
        initialized = true;
        LOG_INFO("TraceRoute initialized for first time");
    }

    unsigned long now = millis();
    if (initialized && lastTraceRouteTime > 0 && Throttle::isWithinTimespanMs(lastTraceRouteTime, cooldownMs)) {
        unsigned long wait = (cooldownMs - (now - lastTraceRouteTime) + 999) / 1000;
        bannerText = String("Wait for ") + String(wait) + String("s");
        runState = TRACEROUTE_STATE_COOLDOWN;
        resultText = "";
        clearResultLines();

        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        LOG_INFO("Cooldown active, please wait %lu seconds before starting a new trace route.", wait);
        return;
    }

    runState = TRACEROUTE_STATE_TRACKING;
    tracingNode = node;
    lastTraceRouteTime = now;
    resultText = "";
    clearResultLines();
    bannerText = String("Tracing ") + getNodeName(node);

    requestFocus();

    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);

    setIntervalFromNow(1000);

    meshtastic_RouteDiscovery req = meshtastic_RouteDiscovery_init_zero;
    LOG_INFO("Creating RouteDiscovery protobuf...");

    meshtastic_MeshPacket *p = router->allocForSending();
    if (p) {
        if (!prepareRequest(p, node)) {
            packetPool.release(p);
            return;
        }
        p->to = node;
        p->decoded.portnum = meshtastic_PortNum_TRACEROUTE_APP;
        p->decoded.want_response = true;

        // Use reliable delivery for traceroute requests (which will be copied to traceroute responses by setReplyTo)
        p->want_ack = true;

        p->decoded.payload.size =
            pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_RouteDiscovery_msg, &req);

        LOG_INFO("Packet allocated successfully: to=0x%08x, portnum=%d, want_response=%d, payload_size=%d", p->to,
                 p->decoded.portnum, p->decoded.want_response, p->decoded.payload.size);

        if (service) {
            service->sendToMesh(p, RX_SRC_USER);
            LOG_INFO("sendToMesh called successfully for trace route to node 0x%08x", node);
        } else {
            LOG_ERROR("MeshService is NULL!");
            packetPool.release(p);
            runState = TRACEROUTE_STATE_RESULT;
            setResultText("Service unavailable");
            resultShowTime = millis();
            tracingNode = 0;
        }
    } else {
        LOG_ERROR("Failed to allocate TraceRoute packet from router");
        runState = TRACEROUTE_STATE_RESULT;
        setResultText("Failed to send");
        resultShowTime = millis();
        tracingNode = 0;
    }
}

bool TraceRouteModule::acceptsMqttPacket(const meshtastic_MeshPacket &packet) const
{
    return packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
           packet.decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP &&
           TraceRoutePolicy::acceptsMqtt(isToUs(&packet), packet.decoded.want_response, packet.from,
                                        packet.decoded.request_id, runState == TRACEROUTE_STATE_TRACKING,
                                        tracingNode, tracingPacketId);
}

bool TraceRouteModule::prepareRequest(meshtastic_MeshPacket *packet, NodeNum node)
{
    mqttTracePossible = false;
    publicBrokerTrace = false;
    targetAckSeen = false;
    brokerEchoSeen = false;
    radioFailureSeen = false;
    mqttRejectionReason = nullptr;
    const auto *target = nodeDB->getMeshNode(node);
    packet->channel = target ? target->channel : channels.getPrimaryIndex();
    if (packet->channel >= channels.getNumChannels() ||
        channels.getByIndex(packet->channel).role == meshtastic_Channel_Role_DISABLED) {
        handleTraceRouteResult("Node channel unavailable");
        return false;
    }
    const bool viaMqtt = target && target->via_mqtt;
#if !MESHTASTIC_EXCLUDE_MQTT
    const auto &settings = channels.getByIndex(packet->channel).settings;
    mqttTracePossible = moduleConfig.mqtt.enabled && mqtt && settings.uplink_enabled && settings.downlink_enabled;
    publicBrokerTrace = mqttTracePossible && mqtt->isUsingDefaultServer() && channels.isDefaultChannel(packet->channel);
#endif
    // A saved contact's origin flag can be stale. The channel decides whether
    // this request also has a broker return path, not the contact's display flag.
    trackingTimeoutMs = TraceRoutePolicy::timeoutMs(mqttTracePossible);
    if (viaMqtt) {
#if !MESHTASTIC_EXCLUDE_MQTT
        if (!moduleConfig.mqtt.enabled || !mqtt) {
            handleTraceRouteResult("MQTT is disabled");
            return false;
        }
        if (!settings.uplink_enabled || !settings.downlink_enabled) {
            handleTraceRouteResult("MQTT uplink/downlink disabled on node channel");
            return false;
        }
#else
        handleTraceRouteResult("MQTT unavailable in this build");
        return false;
#endif
        bannerText = String("Tracing via MQTT: ") + getNodeName(node);
    } else if (mqttTracePossible) {
        bannerText = String("Tracing LoRa/MQTT: ") + getNodeName(node);
    }
    if (publicBrokerTrace)
        LOG_WARN("Traceroute: public MQTT default-key filtering may block port 70; a contact advertisement is not proof of reachability");
    tracingPacketId = packet->id;
    LOG_INFO("Traceroute request id=0x%08x target=0x%08x channel=%u MQTT=%d contact_mqtt=%d timeout=%lu ms", packet->id,
             node, (unsigned)packet->channel, mqttTracePossible, viaMqtt, trackingTimeoutMs);
    return true;
}

void TraceRouteModule::handleRoutingReport(const meshtastic_MeshPacket &packet, const meshtastic_Routing &report)
{
    if (!nodeDB || packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet.decoded.portnum != meshtastic_PortNum_ROUTING_APP || report.which_variant != meshtastic_Routing_error_reason_tag)
        return;
    const bool mqttTransport = packet.via_mqtt ||
                              packet.transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT;
    const auto event = TraceRoutePolicy::routingReport(isToUs(&packet), packet.from, nodeDB->getNodeNum(),
                                                      packet.decoded.request_id, runState == TRACEROUTE_STATE_TRACKING,
                                                      tracingNode, tracingPacketId, mqttTransport,
                                                      report.error_reason != meshtastic_Routing_Error_NONE);
    using TraceRoutePolicy::RoutingReport;
    if (event == RoutingReport::Ignore)
        return;
    if (event == RoutingReport::BrokerEcho) {
        if (!brokerEchoSeen)
            LOG_INFO("Traceroute broker echo id=0x%08x: upload returned; this is NOT a target acknowledgement", tracingPacketId);
        brokerEchoSeen = true;
    } else if (event == RoutingReport::TargetAck) {
        if (!targetAckSeen)
            LOG_INFO("Traceroute target ACK id=0x%08x from=0x%08x viaMQTT=%d; still waiting for RouteDiscovery",
                     tracingPacketId, packet.from, mqttTransport);
        targetAckSeen = true;
        bannerText = "Target acknowledged; waiting for route";
    } else if (event == RoutingReport::TargetError) {
        LOG_WARN("Traceroute target NAK id=0x%08x from=0x%08x error=%d viaMQTT=%d", tracingPacketId, packet.from,
                 report.error_reason, mqttTransport);
        handleTraceRouteResult(report.error_reason == meshtastic_Routing_Error_NO_RESPONSE
                                   ? String("Target received request but did not provide a traceroute")
                                   : String("Target rejected traceroute (error ") + String((int)report.error_reason) + ")");
    } else {
        if (!radioFailureSeen)
            LOG_WARN("Traceroute local send error id=0x%08x error=%d MQTT=%d", tracingPacketId, report.error_reason,
                     mqttTracePossible);
        radioFailureSeen = true;
        const bool rfDeliveryFailure = report.error_reason == meshtastic_Routing_Error_MAX_RETRANSMIT ||
                                       report.error_reason == meshtastic_Routing_Error_NO_ROUTE ||
                                       report.error_reason == meshtastic_Routing_Error_TIMEOUT;
        // RF retries ending do not cancel an independently published MQTT request.
        if (!mqttTracePossible || !rfDeliveryFailure)
            handleTraceRouteResult(String("Traceroute send failed (error ") + String((int)report.error_reason) + ")");
    }
}

void TraceRouteModule::noteMqttReplyRejected(const meshtastic_MeshPacket &packet, const char *reason)
{
    if (packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet.decoded.portnum != meshtastic_PortNum_TRACEROUTE_APP || mqttRejectionReason ||
        !TraceRoutePolicy::matchesResponse(isToUs(&packet), packet.from, packet.decoded.request_id,
                                           runState == TRACEROUTE_STATE_TRACKING, tracingNode, tracingPacketId))
        return;
    // Callers supply static reasons; no allocation in the MQTT receive callback.
    mqttRejectionReason = reason;
    LOG_WARN("Traceroute matching MQTT reply rejected id=0x%08x from=0x%08x: %s", tracingPacketId, packet.from, reason);
}

void TraceRouteModule::handleTraceRouteResult(const String &result)
{
    setResultText(result);
    runState = TRACEROUTE_STATE_RESULT;
    resultShowTime = millis();
    tracingNode = 0;
    tracingPacketId = 0;

    LOG_INFO("TraceRoute result ready, requesting focus. Result: %s", result.c_str());

    setIntervalFromNow(1000);

    requestFocus();
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);

    LOG_INFO("=== TraceRoute handleTraceRouteResult END ===");
}

bool TraceRouteModule::shouldDraw()
{
    bool draw = (runState != TRACEROUTE_STATE_IDLE);
    static TraceRouteRunState lastLoggedState = TRACEROUTE_STATE_IDLE;
    if (runState != lastLoggedState) {
        LOG_INFO("TraceRoute shouldDraw: runState=%d, draw=%d", runState, draw);
        lastLoggedState = runState;
    }
    return draw;
}
#if HAS_SCREEN
void TraceRouteModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    LOG_DEBUG("TraceRoute drawFrame called: runState=%d", runState);

    display->setTextAlignment(TEXT_ALIGN_CENTER);

    if (runState == TRACEROUTE_STATE_TRACKING) {
        display->setFont(FONT_MEDIUM);
        int centerY = y + (display->getHeight() / 2) - (FONT_HEIGHT_MEDIUM / 2);
        display->drawString(display->getWidth() / 2 + x, centerY, bannerText);

    } else if (runState == TRACEROUTE_STATE_RESULT) {
        display->setFont(FONT_MEDIUM);
        display->setTextAlignment(TEXT_ALIGN_LEFT);

        display->drawString(x, y, "Route Result");

        int contentStartY = y + FONT_HEIGHT_MEDIUM + 2; // Add more spacing after title
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->setFont(FONT_SMALL);

        if (resultText.length() > 0) {
            if (resultLinesDirty) {
                rebuildResultLines(display);
            }

            int lineHeight = FONT_HEIGHT_SMALL + 1; // Use proper font height with 1px spacing
            for (size_t i = 0; i < resultLines.size(); i++) {
                int lineY = contentStartY + (i * lineHeight);
                if (lineY + FONT_HEIGHT_SMALL <= display->getHeight()) {
                    display->drawString(x + 2, lineY, resultLines[i]);
                }
            }
        }

    } else if (runState == TRACEROUTE_STATE_COOLDOWN) {
        display->setFont(FONT_MEDIUM);
        int centerY = y + (display->getHeight() / 2) - (FONT_HEIGHT_MEDIUM / 2);
        display->drawString(display->getWidth() / 2 + x, centerY, bannerText);
    }
}
#endif // HAS_SCREEN
int32_t TraceRouteModule::runOnce()
{
    unsigned long now = millis();

    if (runState == TRACEROUTE_STATE_IDLE) {
        return INT32_MAX;
    }

    // Check for tracking timeout
    if (runState == TRACEROUTE_STATE_TRACKING && !Throttle::isWithinTimespanMs(lastTraceRouteTime, trackingTimeoutMs)) {
        LOG_INFO("Traceroute timeout id=0x%08x target=0x%08x after=%lu ms MQTT=%d broker_echo=%d target_ack=%d RF_error=%d",
                 tracingPacketId, tracingNode, trackingTimeoutMs, mqttTracePossible, brokerEchoSeen, targetAckSeen,
                 radioFailureSeen);
        String result = mqttRejectionReason ? String("Traceroute reply rejected: ") + mqttRejectionReason
                        : targetAckSeen     ? String("Target acknowledged, but no traceroute reply")
                                            : String("No traceroute reply within ") + String(trackingTimeoutMs / 1000) + "s";
        if (publicBrokerTrace && !mqttRejectionReason)
            result += " (public MQTT may filter traceroute)";
        handleTraceRouteResult(result);

        setIntervalFromNow(resultDisplayMs);
        return resultDisplayMs;
    }

    // Update cooldown display every second
    if (runState == TRACEROUTE_STATE_COOLDOWN) {
        unsigned long wait = Throttle::isWithinTimespanMs(lastTraceRouteTime, cooldownMs)
                                 ? (cooldownMs - (now - lastTraceRouteTime) + 999) / 1000
                                 : 0;
        if (wait > 0) {
            String newBannerText = String("Wait for ") + String(wait) + String("s");
            bannerText = newBannerText;
            LOG_INFO("TraceRoute cooldown: updating banner to %s", bannerText.c_str());

            // Force flash UI
            requestFocus();
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);

            if (screen) {
                screen->forceDisplay();
            }

            return 1000;
        } else {
            // Cooldown finished
            LOG_INFO("TraceRoute cooldown finished, returning to IDLE");
            runState = TRACEROUTE_STATE_IDLE;
            resultText = "";
            clearResultLines();
            bannerText = "";
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);
            return INT32_MAX;
        }
    }

    if (runState == TRACEROUTE_STATE_RESULT) {
        if (now - resultShowTime >= resultDisplayMs) {
            LOG_INFO("TraceRoute result display timeout, returning to IDLE");
            runState = TRACEROUTE_STATE_IDLE;
            resultText = "";
            clearResultLines();
            bannerText = "";
            tracingNode = 0;
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);
            return INT32_MAX;
        } else {
            return 1000;
        }
    }

    if (runState == TRACEROUTE_STATE_TRACKING) {
        return 1000;
    }

    return INT32_MAX;
}
