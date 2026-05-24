#include "MQTT.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "ServiceEnvelope.h"
#include "configuration.h"
#include "main.h"
#include "mesh/Channels.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/mqtt.pb.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"
#include "mesh-pb-constants.h"
#include "modules/RoutingModule.h"
#if defined(ARCH_ESP32)
#include "../mesh/generated/meshtastic/paxcount.pb.h"
#endif
#include "mesh/generated/meshtastic/remote_hardware.pb.h"
#include "sleep.h"
#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#endif
#if HAS_ETHERNET && defined(USE_WS5500)
#include <ETHClass2.h>
#define ETH ETH2
#endif // HAS_ETHERNET
#include "Default.h"
#if !defined(ARCH_NRF52) || NRF52_USE_JSON
#include "serialization/JSON.h"
#include "serialization/MeshPacketSerializer.h"
#endif
#include <Throttle.h>
#include <assert.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#if defined(ARCH_ESP32)
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include <IPAddress.h>
#include <pb_decode.h>
#if defined(ARCH_PORTDUINO)
#include <netinet/in.h>
#elif !defined(ntohl)
#include <machine/endian.h>
#define ntohl __ntohl
#endif
#include <RTC.h>

MQTT *mqtt;

namespace
{
constexpr int reconnectMax = 5;
constexpr size_t kMaxMqttInboundBytes = 1024;

// FIXME - this size calculation is super sloppy, but it will go away once we dynamically alloc meshpackets
static uint8_t bytes[meshtastic_MqttClientProxyMessage_size + 30]; // 12 for channel name and 16 for nodeid

static bool isMqttServerAddressPrivate = false;
static bool isConnected = false;
static bool s_mqttPublicFloodModeration = false;
static bool s_mqttTxOnlySafeguard = false;
static bool s_mqttNoSocketOpsSafeguard = false;
static bool s_mqttRxOnlySafeguard = false;
static uint32_t s_mqttRxEnableAfterMs = 0;
static uint32_t s_mqttLastRxLoopMs = 0;
static uint32_t s_mqttIngressWindowMs = 0;
static uint16_t s_mqttIngressCountThisWindow = 0;
static uint32_t s_mqttIngressDropCount = 0;
static uint32_t s_mqttLastIngressDropLogMs = 0;
static uint32_t s_mqttInvalidDropCount = 0;
static uint32_t s_mqttLastInvalidDropLogMs = 0;
static uint32_t s_mqttLastBroadcastPassMs = 0;
static uint32_t s_mqttLastOffTargetPassMs = 0;
static uint32_t s_mqttAcceptedCount = 0;
static uint32_t s_mqttLastAcceptedLogMs = 0;
static uint32_t s_mqttRawFallbackCount = 0;
static uint32_t s_mqttLastRawFallbackLogMs = 0;

static uint32_t lastPositionUnavailableWarning = 0;
static const uint32_t POSITION_UNAVAILABLE_WARNING_INTERVAL_MS = 15000; // 15 seconds

bool allowMqttIngressNow()
{
    if (!s_mqttPublicFloodModeration) {
        return true;
    }

    const uint32_t now = millis();
    if (s_mqttIngressWindowMs == 0 || (now - s_mqttIngressWindowMs) >= 1000) {
        s_mqttIngressWindowMs = now;
        s_mqttIngressCountThisWindow = 0;
    }

    // Hard cap public-firehose ingest on CrowPanel to avoid allocator churn.
    constexpr uint16_t kMaxIngressPerSec = 4;
    if (s_mqttIngressCountThisWindow >= kMaxIngressPerSec) {
        s_mqttIngressDropCount++;
        if ((now - s_mqttLastIngressDropLogMs) > 5000) {
            LOG_WARN("MQTT ingress throttling active: dropped=%u in last window", (unsigned)s_mqttIngressDropCount);
            s_mqttLastIngressDropLogMs = now;
            s_mqttIngressDropCount = 0;
        }
        return false;
    }

    s_mqttIngressCountThisWindow++;
    return true;
}

void logInvalidMqttDrop(const char *reason)
{
    s_mqttInvalidDropCount++;
    const uint32_t now = millis();
    if ((now - s_mqttLastInvalidDropLogMs) < 2000) {
        return;
    }
    s_mqttLastInvalidDropLogMs = now;
    if (s_mqttPublicFloodModeration && strcmp(reason, "unsupported payload variant") == 0) {
        LOG_DEBUG("MQTT drop: %s (count=%u)", reason, (unsigned)s_mqttInvalidDropCount);
    } else {
        LOG_WARN("MQTT drop: %s (count=%u)", reason, (unsigned)s_mqttInvalidDropCount);
    }
    s_mqttInvalidDropCount = 0;
}

template <size_t N> inline std::string boundedArrayString(const char (&src)[N])
{
    const size_t n = strnlen(src, N);
    return std::string(src, n);
}

inline bool parseMqttTopicChannelAndGateway(const char *topic, std::string &channelId, std::string &gatewayId)
{
    if (topic == nullptr) {
        return false;
    }
    const std::string t(topic);
    const size_t slashLast = t.rfind('/');
    if (slashLast == std::string::npos || slashLast + 1 >= t.size()) {
        return false;
    }
    const size_t slashPrev = t.rfind('/', slashLast - 1);
    if (slashPrev == std::string::npos || slashPrev + 1 >= slashLast) {
        return false;
    }
    channelId = t.substr(slashPrev + 1, slashLast - slashPrev - 1);
    gatewayId = t.substr(slashLast + 1);
    return !channelId.empty() && !gatewayId.empty();
}

inline bool isHexNibble(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool isLikelyGatewayId(const std::string &gatewayId)
{
    // Expected Meshtastic node-id style in MQTT topics: "!1234abcd"
    if (gatewayId.size() != 9 || gatewayId[0] != '!') {
        return false;
    }
    for (size_t i = 1; i < gatewayId.size(); ++i) {
        if (!isHexNibble(gatewayId[i])) {
            return false;
        }
    }
    return true;
}

inline bool isPlausibleMqttMeshPacket(const meshtastic_MeshPacket &p)
{
    if (p.from == 0 || p.to == 0 || p.id == 0) {
        return false;
    }
    if (p.hop_limit > HOP_MAX || p.hop_start > HOP_MAX) {
        return false;
    }
    if (p.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
        return p.encrypted.size > 0 && p.encrypted.size <= sizeof(p.encrypted.bytes);
    }
    if (p.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        return p.decoded.portnum != meshtastic_PortNum_UNKNOWN_APP;
    }
    return false;
}

inline bool decodeMeshPacketQuiet(const uint8_t *payload, size_t length, meshtastic_MeshPacket *dest)
{
    pb_istream_t stream = pb_istream_from_buffer(payload, length);
    return pb_decode(&stream, &meshtastic_MeshPacket_msg, dest);
}

inline void onReceiveProto(char *topic, byte *payload, size_t length)
{
    if (length == 0 || length > kMaxMqttInboundBytes) {
        logInvalidMqttDrop("proto payload length invalid");
        return;
    }

    const DecodedServiceEnvelope e(payload, length);
    meshtastic_MeshPacket packetFallback = meshtastic_MeshPacket_init_default;
    const meshtastic_MeshPacket *packet = nullptr;
    std::string topicChannelId;
    std::string topicGatewayId;
    if (!parseMqttTopicChannelAndGateway(topic, topicChannelId, topicGatewayId) || !isLikelyGatewayId(topicGatewayId)) {
        logInvalidMqttDrop("invalid topic");
        return;
    }
    const char *channelId = topicChannelId.c_str();
    const char *gatewayId = topicGatewayId.c_str();

    if (e.validDecode && e.channel_id != NULL && e.gateway_id != NULL && e.packet != NULL) {
        // Reject forged/misaligned envelopes early: topic path is authoritative for routing context.
        if (strcmp(e.channel_id, channelId) != 0 || strcmp(e.gateway_id, gatewayId) != 0) {
            logInvalidMqttDrop("topic/envelope mismatch");
            return;
        }
        packet = e.packet;
    } else {
        // Some gateways publish raw MeshPacket payloads on the same topic path.
        // Fall back to MeshPacket decode and derive channel/gateway from topic.
        if (!decodeMeshPacketQuiet(payload, length, &packetFallback)) {
            logInvalidMqttDrop("service envelope decode failed");
            return;
        }
        packet = &packetFallback;
        s_mqttRawFallbackCount++;
        const uint32_t now = millis();
        if ((now - s_mqttLastRawFallbackLogMs) > 5000) {
            LOG_INFO("MQTT raw-packet fallback hits: %u", (unsigned)s_mqttRawFallbackCount);
            s_mqttRawFallbackCount = 0;
            s_mqttLastRawFallbackLogMs = now;
        }
    }

    if (!isPlausibleMqttMeshPacket(*packet)) {
        logInvalidMqttDrop("mesh packet sanity failed");
        return;
    }

    const meshtastic_Channel &ch = channels.getByName(channelId);
    // Find channel by channel_id and check downlink_enabled
    if (!(strcmp(channelId, "PKI") == 0 || (strcmp(channelId, channels.getGlobalId(ch.index)) == 0 && ch.settings.downlink_enabled))) {
        return;
    }

    bool anyChannelHasDownlink = false;
    size_t numChan = channels.getNumChannels();
    for (size_t i = 0; i < numChan; ++i) {
        const auto &c = channels.getByIndex(i);
        if (c.settings.downlink_enabled) {
            anyChannelHasDownlink = true;
            break;
        }
    }

    if (strcmp(channelId, "PKI") == 0 && !anyChannelHasDownlink) {
        return;
    }
    // Generate node ID from nodenum for comparison
    std::string nodeId = nodeDB->getNodeId();
    if (strcmp(gatewayId, nodeId.c_str()) == 0) {
        // Generate an implicit ACK towards ourselves (handled and processed only locally!) for this message.
        // We do this because packets are not rebroadcasted back into MQTT anymore and we assume that at least one node
        // receives it when we get our own packet back. Then we'll stop our retransmissions.
        if (isFromUs(packet)) {
            auto pAck = routingModule->allocAckNak(meshtastic_Routing_Error_NONE, getFrom(packet), packet->id, ch.index);
            pAck->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT;
            router->sendLocal(pAck);
        } else {
            LOG_INFO("Ignore downlink message we originally sent");
        }
        return;
    }
    if (isFromUs(packet)) {
        LOG_INFO("Ignore downlink message we originally sent");
        return;
    }

    if (s_mqttPublicFloodModeration) {
        const uint32_t now = millis();
        const uint32_t myNodeNum = nodeDB->getNodeNum();
        const bool isDirectToUs = (packet->to == myNodeNum);
        const bool isBroadcast = (packet->to == NODENUM_BROADCAST);
        if (!isDirectToUs && !isBroadcast) {
            // Keep a controlled trickle of off-target unicast for node discovery.
            constexpr uint32_t kOffTargetMinGapMs = 500;
            if ((now - s_mqttLastOffTargetPassMs) < kOffTargetMinGapMs) {
                return;
            }
            s_mqttLastOffTargetPassMs = now;
        }
        if (isBroadcast) {
            constexpr uint32_t kBroadcastMinGapMs = 750; // let a small sample through
            if ((now - s_mqttLastBroadcastPassMs) < kBroadcastMinGapMs) {
                return;
            }
            s_mqttLastBroadcastPassMs = now;
        }
        // Throttle only packets that passed destination/channel relevance checks.
        if (!allowMqttIngressNow()) {
            return;
        }
    }

    if (!s_mqttPublicFloodModeration) {
        LOG_DEBUG("Received MQTT topic %s, len=%u", topic, (unsigned)length);
    }
    if (packet->hop_limit > HOP_MAX || packet->hop_start > HOP_MAX) {
        LOG_INFO("Invalid hop_limit(%u) or hop_start(%u)", packet->hop_limit, packet->hop_start);
        return;
    }

    if (packet->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        if (moduleConfig.mqtt.encryption_enabled) {
            LOG_INFO("Ignore decoded message on MQTT, encryption is enabled");
            return;
        }
        if (packet->decoded.portnum == meshtastic_PortNum_ADMIN_APP) {
            LOG_INFO("Ignore decoded admin packet");
            return;
        }
    } else if (packet->which_payload_variant != meshtastic_MeshPacket_encrypted_tag) {
        logInvalidMqttDrop("unsupported payload variant");
        return;
    }

    UniquePacketPoolPacket p = packetPool.allocUniqueZeroed();
    p->from = packet->from;
    p->to = packet->to;
    p->id = packet->id;
    p->channel = packet->channel;
    p->hop_limit = packet->hop_limit;
    p->hop_start = packet->hop_start;
    p->want_ack = packet->want_ack;
    p->via_mqtt = true; // Mark that the packet was received via MQTT
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT;
    p->which_payload_variant = packet->which_payload_variant;

    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        p->decoded = packet->decoded;
    } else {
        p->encrypted = packet->encrypted;
    }

    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        p->channel = ch.index;
    }

    bool delivered = false;
    // PKI messages get accepted even if we can't decrypt
    if (router && p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag && strcmp(channelId, "PKI") == 0) {
        const meshtastic_NodeInfoLite *tx = nodeDB->getMeshNode(getFrom(p.get()));
        const meshtastic_NodeInfoLite *rx = nodeDB->getMeshNode(p->to);
        // Only accept PKI messages to us, or if we have both the sender and receiver in our nodeDB, as then it's
        // likely they discovered each other via a channel we have downlink enabled for
        if (isToUs(p.get()) || (tx && tx->has_user && rx && rx->has_user)) {
            router->enqueueReceivedMessage(p.release());
            delivered = true;
        }
    } else if (router &&
               perhapsDecode(p.get()) == DecodeState::DECODE_SUCCESS) { // ignore messages if we don't have the channel key
        router->enqueueReceivedMessage(p.release());
        delivered = true;
    }

    if (delivered) {
        s_mqttAcceptedCount++;
        const uint32_t now = millis();
        if ((now - s_mqttLastAcceptedLogMs) > 5000) {
            LOG_INFO("MQTT accepted packets: %u", (unsigned)s_mqttAcceptedCount);
            s_mqttAcceptedCount = 0;
            s_mqttLastAcceptedLogMs = now;
        }
    }
}

#if !defined(ARCH_NRF52) || NRF52_USE_JSON
// returns true if this is a valid JSON envelope which we accept on downlink
inline bool isValidJsonEnvelope(JSONObject &json)
{
    // Generate node ID from nodenum for comparison
    std::string nodeId = nodeDB->getNodeId();
    // if "sender" is provided, avoid processing packets we uplinked
    return (json.find("sender") != json.end() ? (json["sender"]->AsString().compare(nodeId) != 0) : true) &&
           (json.find("hopLimit") != json.end() ? json["hopLimit"]->IsNumber() : true) && // hop limit should be a number
           (json.find("from") != json.end()) && json["from"]->IsNumber() &&
           (json["from"]->AsNumber() == nodeDB->getNodeNum()) &&            // only accept message if the "from" is us
           (json.find("type") != json.end()) && json["type"]->IsString() && // should specify a type
           (json.find("payload") != json.end());                            // should have a payload
}

inline void onReceiveJson(byte *payload, size_t length)
{
    if (length == 0 || length > kMaxMqttInboundBytes) {
        LOG_WARN("Drop MQTT JSON payload length=%u", (unsigned)length);
        return;
    }

    std::string payloadStr(reinterpret_cast<const char *>(payload), length);
    std::unique_ptr<JSONValue> json_value(JSON::Parse(payloadStr.c_str()));
    if (json_value == nullptr) {
        LOG_ERROR("JSON received payload on MQTT but not a valid JSON");
        return;
    }

    JSONObject json;
    json = json_value->AsObject();

    if (!isValidJsonEnvelope(json)) {
        LOG_ERROR("JSON received payload on MQTT but not a valid envelope");
        return;
    }

    // this is a valid envelope
    if (json["type"]->AsString().compare("sendtext") == 0 && json["payload"]->IsString()) {
        std::string jsonPayloadStr = json["payload"]->AsString();
        LOG_INFO("JSON payload %s, length %u", jsonPayloadStr.c_str(), jsonPayloadStr.length());

        // construct protobuf data packet using TEXT_MESSAGE, send it to the mesh
        meshtastic_MeshPacket *p = router->allocForSending();
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        if (json.find("channel") != json.end() && json["channel"]->IsNumber() &&
            (json["channel"]->AsNumber() < channels.getNumChannels()))
            p->channel = json["channel"]->AsNumber();
        if (json.find("to") != json.end() && json["to"]->IsNumber())
            p->to = json["to"]->AsNumber();
        if (json.find("hopLimit") != json.end() && json["hopLimit"]->IsNumber())
            p->hop_limit = json["hopLimit"]->AsNumber();
        if (jsonPayloadStr.length() <= sizeof(p->decoded.payload.bytes)) {
            memcpy(p->decoded.payload.bytes, jsonPayloadStr.c_str(), jsonPayloadStr.length());
            p->decoded.payload.size = jsonPayloadStr.length();
            service->sendToMesh(p, RX_SRC_LOCAL);
        } else {
            LOG_WARN("Received MQTT json payload too long, drop");
        }
    } else if (json["type"]->AsString().compare("sendposition") == 0 && json["payload"]->IsObject()) {
        // invent the "sendposition" type for a valid envelope
        JSONObject posit;
        posit = json["payload"]->AsObject(); // get nested JSON Position
        meshtastic_Position pos = meshtastic_Position_init_default;
        if (posit.find("latitude_i") != posit.end() && posit["latitude_i"]->IsNumber())
            pos.latitude_i = posit["latitude_i"]->AsNumber();
        if (posit.find("longitude_i") != posit.end() && posit["longitude_i"]->IsNumber())
            pos.longitude_i = posit["longitude_i"]->AsNumber();
        if (posit.find("altitude") != posit.end() && posit["altitude"]->IsNumber())
            pos.altitude = posit["altitude"]->AsNumber();
        if (posit.find("time") != posit.end() && posit["time"]->IsNumber())
            pos.time = posit["time"]->AsNumber();

        // construct protobuf data packet using POSITION, send it to the mesh
        meshtastic_MeshPacket *p = router->allocForSending();
        p->decoded.portnum = meshtastic_PortNum_POSITION_APP;
        if (json.find("channel") != json.end() && json["channel"]->IsNumber() &&
            (json["channel"]->AsNumber() < channels.getNumChannels()))
            p->channel = json["channel"]->AsNumber();
        if (json.find("to") != json.end() && json["to"]->IsNumber())
            p->to = json["to"]->AsNumber();
        if (json.find("hopLimit") != json.end() && json["hopLimit"]->IsNumber())
            p->hop_limit = json["hopLimit"]->AsNumber();
        p->decoded.payload.size =
            pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Position_msg,
                               &pos); // make the Data protobuf from position
        service->sendToMesh(p, RX_SRC_LOCAL);
    } else {
        LOG_DEBUG("JSON ignore downlink message with unsupported type");
    }
}
#endif

/// Determines if the given IPAddress is a private IPv4 address, i.e. not routable on the public internet.
bool isPrivateIpAddress(const IPAddress &ip)
{
    constexpr struct {
        uint32_t network;
        uint32_t mask;
    } privateCidrRanges[] = {
        {.network = 192u << 24 | 168 << 16, .mask = 0xffff0000}, // 192.168.0.0/16
        {.network = 172u << 24 | 16 << 16, .mask = 0xfff00000},  // 172.16.0.0/12
        {.network = 169u << 24 | 254 << 16, .mask = 0xffff0000}, // 169.254.0.0/16
        {.network = 10u << 24, .mask = 0xff000000},              // 10.0.0.0/8
        {.network = 127u << 24 | 1, .mask = 0xffffffff},         // 127.0.0.1/32
        {.network = 100u << 24 | 64 << 16, .mask = 0xffc00000},  // 100.64.0.0/10
    };
    const uint32_t addr = ntohl(ip);
    for (const auto &cidrRange : privateCidrRanges) {
        if (cidrRange.network == (addr & cidrRange.mask)) {
            LOG_INFO("MQTT server on a private IP");
            return true;
        }
    }
    return false;
}

// Separate a <host>[:<port>] string. Returns a pair containing the parsed host and port. If the port is
// not present in the input string, or is invalid, the value of the `port` argument will be returned.
std::pair<String, uint16_t> parseHostAndPort(String server, uint16_t port = 0)
{
    const int delimIndex = server.indexOf(':');
    if (delimIndex > 0) {
        const long parsedPort = server.substring(delimIndex + 1, server.length()).toInt();
        if (parsedPort < 1 || parsedPort > UINT16_MAX) {
            LOG_WARN("Invalid MQTT port %d: %s", parsedPort, server.c_str());
        } else {
            port = parsedPort;
        }
        server[delimIndex] = 0;
    }
    return std::make_pair(std::move(server), port);
}

bool isDefaultServer(const String &host)
{
    return host.length() == 0 || host == default_mqtt_address;
}

bool isDefaultRootTopic(const String &root)
{
    return root.length() == 0 || root == default_mqtt_root;
}

String normalizeRootTopic(String root)
{
    root.trim();
    while (root.length() > 0 && root.endsWith("/")) {
        root.remove(root.length() - 1);
    }
    return root;
}

struct PubSubConfig {
    explicit PubSubConfig(const meshtastic_ModuleConfig_MQTTConfig &config)
    {
        const std::string cfgAddress = boundedArrayString(config.address);
        const std::string cfgUsername = boundedArrayString(config.username);
        const std::string cfgPassword = boundedArrayString(config.password);

        if (!cfgAddress.empty()) {
            serverAddr = cfgAddress.c_str();
            mqttUsernameStorage = cfgUsername;
            mqttPasswordStorage = cfgPassword;
            mqttUsername = mqttUsernameStorage.empty() ? default_mqtt_username : mqttUsernameStorage.c_str();
            mqttPassword = mqttPasswordStorage.empty() ? default_mqtt_password : mqttPasswordStorage.c_str();
        }
        if (config.tls_enabled) {
            serverPort = 8883;
        }
        useTls = config.tls_enabled;
        auto [parsedServerAddr, parsedServerPort] = parseHostAndPort(serverAddr, serverPort);
        serverAddr = std::move(parsedServerAddr);
        serverPort = parsedServerPort;
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
        // ESP-Hosted on this board is fragile around DNS allocator paths.
        // For the stock Meshtastic broker, use fixed IPv4 to bypass DNS.
        if (serverAddr == default_mqtt_address) {
            serverAddr = "15.204.60.243";
            LOG_WARN("CrowPanel MQTT: using fixed broker IP %s (DNS bypass)", serverAddr.c_str());
        }
#endif
        serverIsIp = serverIp.fromString(serverAddr.c_str());
    }

    // Defaults
    static constexpr uint16_t defaultPort = 1883;
    static constexpr uint16_t defaultPortTls = 8883;

    uint16_t serverPort = defaultPort;
    String serverAddr = default_mqtt_address;
    std::string mqttUsernameStorage;
    std::string mqttPasswordStorage;
    const char *mqttUsername = default_mqtt_username;
    const char *mqttPassword = default_mqtt_password;
    IPAddress serverIp;
    bool serverIsIp = false;
    bool useTls = false;
};

#if HAS_NETWORKING
bool connectPubSub(const PubSubConfig &config, PubSubClient &pubSub, Client &client)
{
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
    // Do not resize PubSub buffers at connect time on this board.
    // On P4 + ESP-Hosted, realloc paths are fragile once display/WiFi are up.
    // Keep constructor/default buffer sizing to avoid connect-time heap churn.
#else
    if (pubSub.getSendBufferSize() != 1024 || pubSub.getReceiveBufferSize() != 1024) {
        if (!pubSub.setBufferSize(1024, 1024)) {
            LOG_ERROR("MQTT: failed to allocate PubSub buffers recv=1024 send=1024");
            return false;
        }
    }
#endif
    pubSub.setClient(client);
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
    // Keepalive writes after framebuffer init can trigger allocator issues on this
    // target. Use a long keepalive to minimize post-boot TX on the MQTT socket.
    pubSub.setKeepAlive(1800); // seconds
    pubSub.setSocketTimeout(1);
#endif
    if (config.serverIsIp) {
        pubSub.setServer(config.serverIp, config.serverPort);
    } else {
        pubSub.setServer(config.serverAddr.c_str(), config.serverPort);
    }

    // NOTE(CrowPanel): Do not manually pre-connect or mutate socket options here.
    // On ESP32-P4 + ESP-Hosted this path has shown allocator instability.
    // Let PubSubClient manage socket open/connect in one path.

    LOG_INFO("Connecting directly to MQTT server %s, port: %d, username: %s",
             config.serverAddr.c_str(), config.serverPort, config.mqttUsername);

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D) && defined(ARCH_ESP32)
    if (!heap_caps_check_integrity_all(false)) {
        LOG_ERROR("CrowPanel MQTT abort: heap integrity check failed before pubSub.connect()");
        return false;
    }
#endif

    // Generate node ID from nodenum for client identification
    std::string nodeId = nodeDB->getNodeId();
    const bool connected = pubSub.connect(nodeId.c_str(), config.mqttUsername, config.mqttPassword);
    if (connected) {
        isConnected = true;
        LOG_INFO("MQTT connected");
    } else {
        isConnected = false;
        LOG_WARN("Failed to connect to MQTT server");
    }
    return connected;
}
#endif

inline bool isConnectedToNetwork()
{
#ifdef USE_WS5500
    if (ETH.connected())
        return true;
#endif

#if HAS_WIFI
    return WiFi.isConnected();
#elif HAS_ETHERNET
    return Ethernet.linkStatus() == LinkON;
#else
    return false;
#endif
}

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
static uint32_t s_crowpanelWifiUpSinceMs = 0;
static uint32_t s_crowpanelLastMqttAttemptMs = 0;
static constexpr uint32_t kCrowpanelMqttStartupDelayMs = 20000;
static constexpr uint32_t kCrowpanelMqttRetryDelayMs = 15000;
static constexpr size_t kCrowpanelMinFreeHeapBytes = 150 * 1024;
static constexpr size_t kCrowpanelMinLargestBlockBytes = 100 * 1024;
static constexpr size_t kCrowpanelMinDmaFreeBytes = 48 * 1024;
static constexpr size_t kCrowpanelMinDmaLargestBlockBytes = 24 * 1024;

static bool crowpanelMqttConnectWindowOpen()
{
    const bool networkUp = isConnectedToNetwork();
    if (!networkUp) {
        s_crowpanelWifiUpSinceMs = 0;
        s_crowpanelLastMqttAttemptMs = 0;
        return false;
    }

    const uint32_t now = millis();
    if (s_crowpanelWifiUpSinceMs == 0) {
        s_crowpanelWifiUpSinceMs = now;
        return false;
    }

    return (now - s_crowpanelWifiUpSinceMs) >= kCrowpanelMqttStartupDelayMs;
}

static bool crowpanelHasHeapHeadroomForMqtt()
{
#if defined(ARCH_ESP32)
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t freeDma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t largestDmaBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (freeHeap < kCrowpanelMinFreeHeapBytes || largestBlock < kCrowpanelMinLargestBlockBytes ||
        freeDma < kCrowpanelMinDmaFreeBytes || largestDmaBlock < kCrowpanelMinDmaLargestBlockBytes) {
        LOG_WARN("CrowPanel MQTT defer: low heap/DMA headroom free=%u largest=%u dma_free=%u dma_largest=%u",
                 (unsigned)freeHeap, (unsigned)largestBlock, (unsigned)freeDma, (unsigned)largestDmaBlock);
        return false;
    }
#endif
    return true;
}
#endif

/** return true if we have a channel that wants uplink/downlink or map reporting is enabled
 */
bool wantsLink()
{
    const bool hasChannelorMapReport =
        moduleConfig.mqtt.enabled && (moduleConfig.mqtt.map_reporting_enabled || channels.anyMqttEnabled());
    return hasChannelorMapReport && (moduleConfig.mqtt.proxy_to_client_enabled || isConnectedToNetwork());
}
} // namespace

void MQTT::mqttCallback(char *topic, byte *payload, unsigned int length)
{
    mqtt->onReceive(topic, payload, length);
}

void MQTT::onClientProxyReceive(meshtastic_MqttClientProxyMessage msg)
{
    onReceive(msg.topic, msg.payload_variant.data.bytes, msg.payload_variant.data.size);
}

void MQTT::onReceive(char *topic, byte *payload, size_t length)
{
    if (length == 0) {
        logInvalidMqttDrop("empty payload");
        return;
    }
    if (length > kMaxMqttInboundBytes) {
        logInvalidMqttDrop("oversized payload");
        return;
    }

    // Topic leaf is uploader/gateway node, not final destination.
    // Do not pre-filter by topic node id here; destination filtering is done
    // after envelope decode in onReceiveProto().

    // check if this is a json payload message by comparing the topic start
    if (moduleConfig.mqtt.json_enabled && (strncmp(topic, jsonTopic.c_str(), jsonTopic.length()) == 0)) {
#if !defined(ARCH_NRF52) || NRF52_USE_JSON
        // parse the channel name from the topic string
        // the topic has been checked above for having jsonTopic prefix, so just move past it
        char *channelName = topic + jsonTopic.length();
        // if another "/" was added, parse string up to that character
        channelName = strtok(channelName, "/") ? strtok(channelName, "/") : channelName;
        // We allow downlink JSON packets only on a channel named "mqtt"
        const meshtastic_Channel &sendChannel = channels.getByName(channelName);
        if (!(strncasecmp(channels.getGlobalId(sendChannel.index), Channels::mqttChannel, strlen(Channels::mqttChannel)) == 0 &&
              sendChannel.settings.downlink_enabled)) {
            LOG_WARN("JSON downlink received on channel not called 'mqtt' or without downlink enabled");
            return;
        }
        onReceiveJson(payload, length);
#endif
        return;
    }

    onReceiveProto(topic, payload, length);
}

void mqttInit()
{
    new MQTT();
}

#if HAS_NETWORKING
MQTT::MQTT() : MQTT(std::unique_ptr<MQTTClient>(new MQTTClient())) {}
MQTT::MQTT(std::unique_ptr<MQTTClient> _mqttClient)
    : concurrency::OSThread("mqtt"), mqttQueue(MAX_MQTT_QUEUE), mqttClient(std::move(_mqttClient)), pubSub(*mqttClient)
#else
MQTT::MQTT() : concurrency::OSThread("mqtt"), mqttQueue(MAX_MQTT_QUEUE)
#endif
{
    if (moduleConfig.mqtt.enabled) {
        LOG_DEBUG("Init MQTT");

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
        if (moduleConfig.mqtt.proxy_to_client_enabled) {
            LOG_WARN("CrowPanel P4: disabling MQTT client proxy and using direct MQTT");
            moduleConfig.mqtt.proxy_to_client_enabled = false;
        }
#endif

        assert(!mqtt);
        mqtt = this;

        const String configuredRoot = normalizeRootTopic(String(boundedArrayString(moduleConfig.mqtt.root).c_str()));
        if (configuredRoot.length() > 0) {
            cryptTopic = std::string(configuredRoot.c_str()) + cryptTopic;
            jsonTopic = std::string(configuredRoot.c_str()) + jsonTopic;
            mapTopic = std::string(configuredRoot.c_str()) + mapTopic;
            isConfiguredForDefaultRootTopic = isDefaultRootTopic(configuredRoot);
        } else {
            cryptTopic = "msh" + cryptTopic;
            jsonTopic = "msh" + jsonTopic;
            mapTopic = "msh" + mapTopic;
            isConfiguredForDefaultRootTopic = true;
        }

        if (moduleConfig.mqtt.map_reporting_enabled && moduleConfig.mqtt.has_map_report_settings) {
            map_position_precision = Default::getConfiguredOrDefault(moduleConfig.mqtt.map_report_settings.position_precision,
                                                                     default_map_position_precision);
            map_publish_interval_msecs = Default::getConfiguredOrDefaultMs(
                moduleConfig.mqtt.map_report_settings.publish_interval_secs, default_map_publish_interval_secs);
        }

        const String configuredAddress = String(boundedArrayString(moduleConfig.mqtt.address).c_str());
        auto [host, parsedPort] = parseHostAndPort(configuredAddress);
        (void)parsedPort;
        isConfiguredForDefaultServer = isDefaultServer(host);
        IPAddress ip;
        isMqttServerAddressPrivate = ip.fromString(host.c_str()) && isPrivateIpAddress(ip);

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
        // CrowPanel P4 safety mode: always enable strict flood moderation.
        // This target is memory-constrained under public MQTT firehose traffic.
        s_mqttPublicFloodModeration = true;
        // Recovery profile:
        // - RX only (no uplink publishing)
        // - delayed + sparse MQTT loop polling to reduce allocator stress
        s_mqttRxOnlySafeguard = true;
        s_mqttTxOnlySafeguard = false;
        s_mqttNoSocketOpsSafeguard = false;
        s_mqttRxEnableAfterMs = millis() + 45000U; // Let display + hosted stack settle first.
        s_mqttLastRxLoopMs = 0;
        LOG_WARN("CrowPanel MQTT moderation=%d (defaultServer=%d defaultRoot=%d host=%s root=%s)",
                 s_mqttPublicFloodModeration ? 1 : 0,
                 isConfiguredForDefaultServer ? 1 : 0,
                 isConfiguredForDefaultRootTopic ? 1 : 0,
                 host.c_str(),
                 configuredRoot.c_str());
        LOG_WARN("CrowPanel MQTT rx-only safeguard=%d", s_mqttRxOnlySafeguard ? 1 : 0);
        LOG_WARN("CrowPanel MQTT tx-only safeguard=%d", s_mqttTxOnlySafeguard ? 1 : 0);
        LOG_WARN("CrowPanel MQTT no-socket safeguard=%d", s_mqttNoSocketOpsSafeguard ? 1 : 0);
#endif

#if HAS_NETWORKING
        if (!moduleConfig.mqtt.proxy_to_client_enabled)
            pubSub.setCallback(mqttCallback);
#endif

        if (moduleConfig.mqtt.proxy_to_client_enabled) {
            LOG_INFO("MQTT configured to use client proxy");
            enabled = true;
            runASAP = true;
            reconnectCount = 0;
#if !IS_RUNNING_TESTS
            publishNodeInfo();
#endif
        }
        // preflightSleepObserver.observe(&preflightSleep);
    } else {
        disable();
    }
}

bool MQTT::isConnectedDirectly()
{
#if HAS_NETWORKING
    return pubSub.connected();
#else
    return false;
#endif
}

bool MQTT::publish(const char *topic, const char *payload, bool retained)
{
    if (moduleConfig.mqtt.proxy_to_client_enabled) {
        meshtastic_MqttClientProxyMessage *msg = mqttClientProxyMessagePool.allocZeroed();
        msg->which_payload_variant = meshtastic_MqttClientProxyMessage_text_tag;
        strncpy(msg->topic, topic, sizeof(msg->topic));
        msg->topic[sizeof(msg->topic) - 1] = '\0';
        strncpy(msg->payload_variant.text, payload, sizeof(msg->payload_variant.text));
        msg->payload_variant.text[sizeof(msg->payload_variant.text) - 1] = '\0';
        msg->retained = retained;
        service->sendMqttMessageToClientProxy(msg);
        return true;
    }
#if HAS_NETWORKING
    else if (isConnectedDirectly()) {
        return pubSub.publish(topic, payload, retained);
    }
#endif
    return false;
}

bool MQTT::publish(const char *topic, const uint8_t *payload, size_t length, bool retained)
{
    if (moduleConfig.mqtt.proxy_to_client_enabled) {
        meshtastic_MqttClientProxyMessage *msg = mqttClientProxyMessagePool.allocZeroed();
        msg->which_payload_variant = meshtastic_MqttClientProxyMessage_data_tag;
        strncpy(msg->topic, topic, sizeof(msg->topic));
        msg->topic[sizeof(msg->topic) - 1] = '\0'; // Ensure null termination
        if (length > sizeof(msg->payload_variant.data.bytes))
            length = sizeof(msg->payload_variant.data.bytes);
        msg->payload_variant.data.size = length;
        memcpy(msg->payload_variant.data.bytes, payload, length);
        msg->retained = retained;
        service->sendMqttMessageToClientProxy(msg);
        return true;
    }
#if HAS_NETWORKING
    else if (isConnectedDirectly()) {
        return pubSub.publish(topic, payload, length, retained);
    }
#endif
    return false;
}

void MQTT::reconnect()
{
    isConnected = false;
    if (wantsLink()) {
        if (moduleConfig.mqtt.proxy_to_client_enabled) {
            LOG_INFO("MQTT connect via client proxy instead");
            enabled = true;
            runASAP = true;
            reconnectCount = 0;

            publishNodeInfo();
            return; // Don't try to connect directly to the server
        }
#if HAS_NETWORKING
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
        // ESP-Hosted on P4/C6 is sensitive during the first seconds after STA
        // comes up. Delay direct MQTT connect attempts until WiFi is stable.
        if (!crowpanelMqttConnectWindowOpen()) {
            LOG_WARN("MQTTDBG reconnect defer: connect window not open yet");
            return;
        }
        if (!crowpanelHasHeapHeadroomForMqtt()) {
            LOG_WARN("MQTTDBG reconnect defer: insufficient headroom");
            return;
        }
        const uint32_t nowMs = millis();
        if (s_crowpanelLastMqttAttemptMs != 0 && (nowMs - s_crowpanelLastMqttAttemptMs) < kCrowpanelMqttRetryDelayMs) {
            LOG_WARN("MQTTDBG reconnect defer: retry throttle");
            return;
        }
        s_crowpanelLastMqttAttemptMs = nowMs;
        const UBaseType_t hwmWords = uxTaskGetStackHighWaterMark(nullptr);
        LOG_WARN("MQTTDBG reconnect attempt starting (task stack free ~= %u bytes)",
                 (unsigned)(hwmWords * sizeof(StackType_t)));
#endif
        const PubSubConfig ps_config(moduleConfig.mqtt);
        MQTTClient *clientConnection = mqttClient.get();
#if MQTT_SUPPORTS_TLS
        if (moduleConfig.mqtt.tls_enabled) {
            mqttClientTLS.setInsecure();
            LOG_INFO("Use TLS-encrypted session");
            clientConnection = &mqttClientTLS;
        } else {
            LOG_INFO("Use non-TLS-encrypted session");
        }
#endif
        if (connectPubSub(ps_config, pubSub, *clientConnection)) {
            enabled = true; // Start running background process again
            runASAP = true;
            reconnectCount = 0;
            isMqttServerAddressPrivate = isPrivateIpAddress(clientConnection->remoteIP());
            isConnected = true;
            publishNodeInfo();
            sendSubscriptions();
        } else {
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
            reconnectCount++;
            LOG_ERROR("Failed to contact MQTT server directly (%d/%d)", reconnectCount, reconnectMax);
            if (reconnectCount >= reconnectMax) {
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
                // On ESP-Hosted, forcing a WiFi reconnect from MQTT failures
                // can destabilize SDIO transport. Keep WiFi up and retry MQTT.
                reconnectCount = reconnectMax - 1;
#else
                needReconnect = true;
                wifiReconnect->setIntervalFromNow(0);
                reconnectCount = 0;
#endif
            }
#endif
        }
#endif
    }
}

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
bool MQTT::prewarmConnectNow()
{
#if HAS_NETWORKING
    if (s_mqttNoSocketOpsSafeguard) {
        LOG_WARN("CrowPanel MQTT: no-socket safeguard active, skip prewarm connect");
        return false;
    }
    if (!moduleConfig.mqtt.enabled || moduleConfig.mqtt.proxy_to_client_enabled)
        return false;
    if (!wantsLink())
        return false;
    if (pubSub.connected()) {
        isConnected = true;
        return true;
    }

    const PubSubConfig ps_config(moduleConfig.mqtt);
    MQTTClient *clientConnection = mqttClient.get();
#if MQTT_SUPPORTS_TLS
    if (moduleConfig.mqtt.tls_enabled) {
        mqttClientTLS.setInsecure();
        LOG_INFO("Use TLS-encrypted session");
        clientConnection = &mqttClientTLS;
    } else {
        LOG_INFO("Use non-TLS-encrypted session");
    }
#endif
    LOG_WARN("CrowPanel MQTT: prewarm connect (pre-framebuffer, clean heap)");
    if (connectPubSub(ps_config, pubSub, *clientConnection)) {
        enabled = true;
        runASAP = true;
        reconnectCount = 0;
        isMqttServerAddressPrivate = isPrivateIpAddress(clientConnection->remoteIP());
        isConnected = true;
        publishNodeInfo();
        sendSubscriptions();
        return true;
    }
    LOG_WARN("CrowPanel MQTT: prewarm connect failed (periodic reconnect will retry)");
    return false;
#else
    return false;
#endif
}
#endif

void MQTT::sendSubscriptions()
{
#if HAS_NETWORKING
    if (s_mqttTxOnlySafeguard) {
        LOG_WARN("CrowPanel MQTT: tx-only safeguard active, skip downlink subscriptions");
        return;
    }

    bool hasDownlink = false;
    const uint8_t subQos = s_mqttRxOnlySafeguard ? 0 : 1;
    size_t numChan = channels.getNumChannels();
    for (size_t i = 0; i < numChan; i++) {
        const auto &ch = channels.getByIndex(i);
        if (ch.settings.downlink_enabled) {
            hasDownlink = true;
            std::string topic = cryptTopic + channels.getGlobalId(i) + "/+";
            LOG_INFO("Subscribe to %s", topic.c_str());
            pubSub.subscribe(topic.c_str(), subQos);
#if !defined(ARCH_NRF52) ||                                                                                                      \
    defined(NRF52_USE_JSON) // JSON is not supported on nRF52, see issue #2804 ### Fixed by using ArduinoJSON ###
            if (moduleConfig.mqtt.json_enabled == true) {
                std::string topicDecoded = jsonTopic + channels.getGlobalId(i) + "/+";
                LOG_INFO("Subscribe to %s", topicDecoded.c_str());
                pubSub.subscribe(topicDecoded.c_str(), subQos);
            }
#endif // ARCH_NRF52 NRF52_USE_JSON
        }
    }
#if !MESHTASTIC_EXCLUDE_PKI
    if (hasDownlink) {
        std::string topic = cryptTopic + "PKI/+";
        LOG_INFO("Subscribe to %s", topic.c_str());
        pubSub.subscribe(topic.c_str(), subQos);
    }
#endif
#endif
}

int32_t MQTT::runOnce()
{
    if (!moduleConfig.mqtt.enabled || !(moduleConfig.mqtt.map_reporting_enabled || channels.anyMqttEnabled()))
        return disable();
    bool wantConnection = wantsLink();

    perhapsReportToMap();

    // If connected poll rapidly, otherwise only occasionally check for a wifi connection change and ability to contact server
    if (moduleConfig.mqtt.proxy_to_client_enabled) {
        publishQueuedMessages();
        return 200;
    }
#if HAS_NETWORKING
    if (s_mqttRxOnlySafeguard) {
        if (!wantConnection)
            return 5000;

        const uint32_t now = millis();
        if (s_mqttRxEnableAfterMs != 0 && now < s_mqttRxEnableAfterMs) {
            return 1000;
        }

        // In rx-only safeguard we never reconnect after boot. Any reconnect path
        // requires socket writes/allocations that are unstable after framebuffer init.
        if (!isConnectedDirectly())
            return 5000;

        // Moderated polling on P4: frequent enough to escape junk-first packets,
        // but bounded to avoid heavy socket churn.
        constexpr uint32_t kRxLoopIntervalMs = 1000;
        if (s_mqttLastRxLoopMs == 0 || (now - s_mqttLastRxLoopMs) >= kRxLoopIntervalMs) {
            s_mqttLastRxLoopMs = now;
            // PubSubClient processes one MQTT frame per loop() call. Run a small burst
            // so a single unsupported frame doesn't stall all useful downlink traffic.
            constexpr int kRxLoopBurst = 6;
            for (int i = 0; i < kRxLoopBurst; ++i) {
                if (!pubSub.loop()) {
                    isConnected = false;
                    return 30000;
                }
            }
            // Allow controlled uplink in guarded mode (single-threaded, one queued packet per tick).
            publishQueuedMessages();
        }
        return 500;
    }

    if (s_mqttNoSocketOpsSafeguard) {
        // Keep thread alive but perform no socket I/O at runtime.
        // This avoids allocator asserts observed in NetworkClient/lwIP paths.
        isConnected = false;
        return 5000;
    }

    if (s_mqttTxOnlySafeguard) {
        if (!wantConnection)
            return 5000;

        if (!isConnectedDirectly()) {
            reconnect();
            if (isConnectedDirectly()) {
                publishQueuedMessages();
                // Keep socket lifetime short on this unstable path.
                pubSub.disconnect();
                isConnected = false;
                return 5000;
            }
            return 30000;
        }

        publishQueuedMessages();
        pubSub.disconnect();
        isConnected = false;
        return 5000;
    }

    else if (!pubSub.loop()) {
        if (!wantConnection)
            return 5000; // If we don't want connection now, check again in 5 secs
        else {
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
            if (!crowpanelMqttConnectWindowOpen()) {
                // Poll quickly while waiting for hosted WiFi link to settle.
                return 1000;
            }
#endif
            reconnect();
            // If we succeeded, empty the queue one by one and start reading rapidly, else try again in 30 seconds (TCP
            // connections are EXPENSIVE so try rarely)
            if (isConnectedDirectly()) {
                publishQueuedMessages();
                return 200;
            } else
                return 30000;
        }
    } else {
        // we are connected to server, check often for new requests on the TCP port
        if (!wantConnection) {
            LOG_INFO("MQTT link not needed, drop");
            pubSub.disconnect();
        }

        powerFSM.trigger(EVENT_CONTACT_FROM_PHONE); // Suppress entering light sleep (because that would turn off bluetooth)
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
        if (s_mqttPublicFloodModeration) {
            // Reduce socket churn on ESP-Hosted path.
            return 150;
        }
#endif
        return 20;
    }
#else
    // No networking available, return default interval
    return 30000;
#endif
}

bool MQTT::isValidConfig(const meshtastic_ModuleConfig_MQTTConfig &config, MQTTClient *client)
{
    const PubSubConfig parsed(config);

    if (config.enabled && !config.proxy_to_client_enabled) {
#if HAS_NETWORKING
        std::unique_ptr<MQTTClient> clientConnection;
        if (config.tls_enabled) {
#if MQTT_SUPPORTS_TLS
            MQTTClientTLS *tlsClient = new MQTTClientTLS;
            clientConnection.reset(tlsClient);
            tlsClient->setInsecure();
#else
            LOG_ERROR("Invalid MQTT config: tls_enabled is not supported on this node");
            return false;
#endif
        } else {
            clientConnection.reset(new MQTTClient);
        }
        std::unique_ptr<PubSubClient> pubSub(new PubSubClient);
        if (isConnectedToNetwork()) {
            return connectPubSub(parsed, *pubSub, (client != nullptr) ? *client : *clientConnection);
        }
#else
        const char *warning = "Invalid MQTT config: proxy_to_client_enabled must be enabled on nodes that do not have a network";
        LOG_ERROR(warning);
#if !IS_RUNNING_TESTS
        meshtastic_ClientNotification *cn = clientNotificationPool.allocZeroed();
        cn->level = meshtastic_LogRecord_Level_ERROR;
        cn->time = getValidTime(RTCQualityFromNet);
        strncpy(cn->message, warning, sizeof(cn->message) - 1);
        cn->message[sizeof(cn->message) - 1] = '\0'; // Ensure null termination
        service->sendClientNotification(cn);
#endif
        return false;
#endif
    }

    const bool defaultServer = isDefaultServer(parsed.serverAddr);
    if (defaultServer && !IS_ONE_OF(parsed.serverPort, PubSubConfig::defaultPort, PubSubConfig::defaultPortTls)) {
        const char *warning = "Invalid MQTT config: default server address must not have a port specified";
        LOG_ERROR(warning);
#if !IS_RUNNING_TESTS
        meshtastic_ClientNotification *cn = clientNotificationPool.allocZeroed();
        cn->level = meshtastic_LogRecord_Level_ERROR;
        cn->time = getValidTime(RTCQualityFromNet);
        strncpy(cn->message, warning, sizeof(cn->message) - 1);
        cn->message[sizeof(cn->message) - 1] = '\0'; // Ensure null termination
        service->sendClientNotification(cn);
#endif
        return false;
    }
    return true;
}

void MQTT::publishNodeInfo()
{
    // TODO: NodeInfo broadcast over MQTT only (NODENUM_BROADCAST_NO_LORA)
}
void MQTT::publishQueuedMessages()
{
    if (mqttQueue.isEmpty())
        return;

    if (!moduleConfig.mqtt.proxy_to_client_enabled && !isConnected)
        return;

    LOG_DEBUG("Publish enqueued MQTT message");
    const std::unique_ptr<QueueEntry> entry(mqttQueue.dequeuePtr(0));
    LOG_INFO("publish %s, %u bytes from queue", entry->topic.c_str(), entry->envBytes.size());
    publish(entry->topic.c_str(), entry->envBytes.data(), entry->envBytes.size(), false);

#if !defined(ARCH_NRF52) ||                                                                                                      \
    defined(NRF52_USE_JSON) // JSON is not supported on nRF52, see issue #2804 ### Fixed by using ArduinoJson ###
    if (!moduleConfig.mqtt.json_enabled)
        return;

    // handle json topic
    const DecodedServiceEnvelope env(entry->envBytes.data(), entry->envBytes.size());
    if (!env.validDecode || env.packet == NULL || env.channel_id == NULL)
        return;

    auto jsonString = MeshPacketSerializer::JsonSerialize(env.packet);
    if (jsonString.length() == 0)
        return;

    // Generate node ID from nodenum for topic
    std::string nodeId = nodeDB->getNodeId();

    std::string topicJson;
    if (env.packet->pki_encrypted) {
        topicJson = jsonTopic + "PKI/" + nodeId;
    } else {
        topicJson = jsonTopic + env.channel_id + "/" + nodeId;
    }
    LOG_INFO("JSON publish message to %s, %u bytes: %s", topicJson.c_str(), jsonString.length(), jsonString.c_str());
    publish(topicJson.c_str(), jsonString.c_str(), false);
#endif // ARCH_NRF52 NRF52_USE_JSON
}

void MQTT::onSend(const meshtastic_MeshPacket &mp_encrypted, const meshtastic_MeshPacket &mp_decoded, ChannelIndex chIndex)
{
    if (s_mqttNoSocketOpsSafeguard)
        return;

    if (mp_encrypted.via_mqtt)
        return; // Don't send messages that came from MQTT back into MQTT
    bool uplinkEnabled = false;
    for (int i = 0; i <= 7; i++) {
        if (channels.getByIndex(i).settings.uplink_enabled)
            uplinkEnabled = true;
    }
    if (!uplinkEnabled)
        return; // no channels have an uplink enabled
    auto &ch = channels.getByIndex(chIndex);

    // mp_decoded will not be decoded when it's PKI encrypted and not directed to us
    if (mp_decoded.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        // For uplinking other's packets, check if it's not OK to MQTT or if it's an older packet without the bitfield
        bool dontUplink = !mp_decoded.decoded.has_bitfield || !(mp_decoded.decoded.bitfield & BITFIELD_OK_TO_MQTT_MASK);
        // Respect the DontMqttMeBro flag for other nodes' packets on public MQTT servers
        if (!isFromUs(&mp_decoded) && !isMqttServerAddressPrivate && dontUplink) {
            LOG_INFO("MQTT onSend - Not forwarding packet due to DontMqttMeBro flag");
            return;
        }

        if (isConfiguredForDefaultServer && (mp_decoded.decoded.portnum == meshtastic_PortNum_RANGE_TEST_APP ||
                                             mp_decoded.decoded.portnum == meshtastic_PortNum_DETECTION_SENSOR_APP)) {
            LOG_DEBUG("MQTT onSend - Ignoring range test or detection sensor message on public mqtt");
            return;
        }
    }
    // Either encrypted packet (we couldn't decrypt) is marked as pki_encrypted, or we could decode the PKI encrypted packet
    bool isPKIEncrypted = mp_encrypted.pki_encrypted || mp_decoded.pki_encrypted;
    // If it was to a channel, check uplink enabled, else must be pki_encrypted
    if (!(ch.settings.uplink_enabled || isPKIEncrypted))
        return;
    const char *channelId = isPKIEncrypted ? "PKI" : channels.getGlobalId(chIndex);

    LOG_DEBUG("MQTT onSend - Publish ");
    const meshtastic_MeshPacket *p;
    if (moduleConfig.mqtt.encryption_enabled) {
        p = &mp_encrypted;
        LOG_DEBUG("encrypted message");
    } else if (mp_decoded.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        p = &mp_decoded;
        LOG_DEBUG("portnum %i message", mp_decoded.decoded.portnum);
    } else {
        LOG_DEBUG("nothing, pkt not decrypted");
        return; // Don't upload a still-encrypted PKI packet if not encryption_enabled
    }

    // Generate node ID from nodenum for service envelope
    std::string nodeId = nodeDB->getNodeId();

    const meshtastic_ServiceEnvelope env = {.packet = const_cast<meshtastic_MeshPacket *>(p),
                                            .channel_id = const_cast<char *>(channelId),
                                            .gateway_id = const_cast<char *>(nodeId.c_str())};
    size_t numBytes = pb_encode_to_bytes(bytes, sizeof(bytes), &meshtastic_ServiceEnvelope_msg, &env);
    std::string topic = cryptTopic + channelId + "/" + nodeId;

    // Always enqueue and let MQTT::runOnce() (single thread) perform socket I/O.
    // This avoids concurrent PubSubClient/NetworkClient access from multiple tasks.
    QueueEntry *entry;
    if (mqttQueue.numFree() == 0) {
        LOG_WARN("MQTT queue is full, discard oldest");
        entry = mqttQueue.dequeuePtr(0);
    } else {
        entry = new QueueEntry;
    }
    entry->topic = std::move(topic);
    entry->envBytes.assign(bytes, numBytes);
    if (mqttQueue.enqueue(entry, 0) == false) {
        LOG_CRIT("Failed to add a message to mqttQueue!");
        abort();
    }
}

void MQTT::perhapsReportToMap()
{
    if (!moduleConfig.mqtt.map_reporting_enabled || !moduleConfig.mqtt.map_report_settings.should_report_location ||
        !(moduleConfig.mqtt.proxy_to_client_enabled || isConnectedDirectly()))
        return;

    // Coerce the map position precision to be within the valid range
    // This removes obtusely large radius and privacy problematic ones from the map
    if (map_position_precision < 12 || map_position_precision > 15) {
        LOG_WARN("MQTT Map report position precision %u is out of range, using default %u", map_position_precision,
                 default_map_position_precision);
        map_position_precision = default_map_position_precision;
    }

    if (Throttle::isWithinTimespanMs(last_report_to_map, map_publish_interval_msecs) && last_report_to_map != 0)
        return;

    if (localPosition.latitude_i == 0 && localPosition.longitude_i == 0) {
        if (Throttle::isWithinTimespanMs(lastPositionUnavailableWarning, POSITION_UNAVAILABLE_WARNING_INTERVAL_MS) == false) {
            LOG_WARN("MQTT Map report enabled, but no position available");
            lastPositionUnavailableWarning = millis();
        }
        return;
    }

    // Allocate MeshPacket and fill it
    meshtastic_MeshPacket *mp = packetPool.allocZeroed();
    mp->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp->from = nodeDB->getNodeNum();
    mp->to = NODENUM_BROADCAST;
    mp->decoded.portnum = meshtastic_PortNum_MAP_REPORT_APP;

    // Fill MapReport message
    meshtastic_MapReport mapReport = meshtastic_MapReport_init_default;
    memcpy(mapReport.long_name, owner.long_name, sizeof(owner.long_name));
    memcpy(mapReport.short_name, owner.short_name, sizeof(owner.short_name));
    mapReport.role = config.device.role;
    mapReport.hw_model = owner.hw_model;
    strncpy(mapReport.firmware_version, optstr(APP_VERSION), sizeof(mapReport.firmware_version));
    mapReport.region = config.lora.region;
    mapReport.modem_preset = config.lora.modem_preset;
    mapReport.has_default_channel = channels.hasDefaultChannel();
    mapReport.has_opted_report_location = true;

    // Set position with precision (same as in PositionModule)
    mapReport.latitude_i = localPosition.latitude_i & (UINT32_MAX << (32 - map_position_precision));
    mapReport.longitude_i = localPosition.longitude_i & (UINT32_MAX << (32 - map_position_precision));
    mapReport.latitude_i += (1 << (31 - map_position_precision));
    mapReport.longitude_i += (1 << (31 - map_position_precision));

    mapReport.altitude = localPosition.altitude;
    mapReport.position_precision = map_position_precision;

    mapReport.num_online_local_nodes = nodeDB->getNumOnlineMeshNodes(true);

    // Encode MapReport message into the MeshPacket
    mp->decoded.payload.size =
        pb_encode_to_bytes(mp->decoded.payload.bytes, sizeof(mp->decoded.payload.bytes), &meshtastic_MapReport_msg, &mapReport);

    // Generate node ID from nodenum for service envelope
    std::string nodeId = nodeDB->getNodeId();

    // Encode the MeshPacket into a binary ServiceEnvelope and publish
    const meshtastic_ServiceEnvelope se = {
        .packet = mp,
        .channel_id = (char *)channels.getGlobalId(channels.getPrimaryIndex()), // Use primary channel as the channel_id
        .gateway_id = const_cast<char *>(nodeId.c_str())};
    size_t numBytes = pb_encode_to_bytes(bytes, sizeof(bytes), &meshtastic_ServiceEnvelope_msg, &se);

    LOG_INFO("MQTT Publish map report to %s", mapTopic.c_str());
    publish(mapTopic.c_str(), bytes, numBytes, false);

    // Release the allocated memory for MeshPacket
    packetPool.release(mp);

    // Update the last report time
    last_report_to_map = millis();
}
