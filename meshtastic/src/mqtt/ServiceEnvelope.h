#pragma once

#include "mesh/generated/meshtastic/mqtt.pb.h"

// Stack-backed ServiceEnvelope decoder. The generated nanopb type uses pointer
// fields, which makes every MQTT packet malloc/free packet/channel/gateway
// storage. On ESP32-P4 + ESP-Hosted that allocator churn is exactly where the
// public MQTT path trips TLSF asserts, so receive-side decode uses fixed storage.
struct DecodedServiceEnvelope {
    DecodedServiceEnvelope(const uint8_t *payload, size_t length);
    DecodedServiceEnvelope(DecodedServiceEnvelope &) = delete;
    DecodedServiceEnvelope(DecodedServiceEnvelope &&);

    meshtastic_MeshPacket *packet = nullptr;
    char *channel_id = nullptr;
    char *gateway_id = nullptr;

    meshtastic_MeshPacket packetStorage = meshtastic_MeshPacket_init_zero;
    char channelStorage[32] = {};
    char gatewayStorage[16] = {};

    // Clients must check that this is true before using.
    bool validDecode = false;
};
