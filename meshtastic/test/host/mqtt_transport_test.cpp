#include "mqtt/MqttFrameBuffer.h"
#include "mqtt/MqttTextPolicy.h"

#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

using Buffer = MqttFrameBuffer<1024>;

static void feed(Buffer &buffer, const std::vector<uint8_t> &packet, size_t chunk = 1024)
{
    for (size_t pos = 0; pos < packet.size();) {
        const size_t n = std::min({chunk, buffer.writeSize(), packet.size() - pos});
        if (n == 0)
            break;
        memcpy(buffer.writeData(), packet.data() + pos, n);
        buffer.commit(n);
        pos += n;
        if (pos < packet.size())
            assert(buffer.available() == 0);
    }
}

static void checkPacket(const std::vector<uint8_t> &packet, size_t chunk)
{
    Buffer buffer;
    feed(buffer, packet, chunk);
    assert(!buffer.failed());
    assert(buffer.available() == packet.size());
    assert(buffer.peek() == packet.front());
    std::vector<uint8_t> result(packet.size());
    for (size_t i = 0; i < result.size(); i++)
        assert(buffer.read(&result[i], 1) == 1);
    assert(result == packet);
    assert(buffer.available() == 0);
    assert(buffer.peek() == -1);
    assert(buffer.writeSize() == 1);
    assert(!buffer.partial());
}

static std::vector<uint8_t> publish(size_t payloadSize, bool qos1 = false)
{
    std::vector<uint8_t> packet{uint8_t(qos1 ? 0x32 : 0x30)};
    size_t remaining = 3 + payloadSize + (qos1 ? 2 : 0);
    do {
        uint8_t digit = remaining % 128;
        remaining /= 128;
        packet.push_back(digit | (remaining ? 128 : 0));
    } while (remaining);
    packet.insert(packet.end(), {0, 1, 't'});
    if (qos1)
        packet.insert(packet.end(), {0, 1});
    packet.resize(packet.size() + payloadSize, 'x');
    return packet;
}

int main()
{
    checkPacket({0x20, 2, 0, 0}, 1); // fragmented CONNACK
    checkPacket({0xd0, 0}, 1);       // empty PINGRESP
    checkPacket(publish(90), 1);
    checkPacket(publish(200, true), 7);
    checkPacket(publish(1018), 1024); // exact buffer capacity

    for (const auto &packet : std::vector<std::vector<uint8_t>>{
             {0x30, 0xff, 0xff, 0xff, 0xff}, // invalid remaining length
             {0x30, 0xff, 0x7f},             // too large
             {0x30, 0},                     // missing topic length
             {0x30, 2, 0, 3},               // truncated topic
             {0x30, 2, 0, 0},               // empty topic
             {0x36, 3, 0, 1, 't'},          // reserved QoS
             {0x32, 3, 0, 1, 't'}}) {       // missing QoS1 packet identifier
        Buffer buffer;
        feed(buffer, packet, 1);
        assert(buffer.failed());
        assert(buffer.available() == 0);
        buffer.reset();
        feed(buffer, {0xd0, 0});
        assert(buffer.available() == 2);
    }

    Buffer partial;
    feed(partial, {0x20, 2, 0});
    assert(partial.partial() && partial.available() == 0);
    assert(partial.read(nullptr, 1) == -1);
    partial.reset();
    feed(partial, {0xd0, 0});
    assert(partial.available() == 2);

    std::mt19937 random(12345);
    for (size_t i = 0; i < 20000; i++)
        checkPacket(publish(random() % 1000, random() % 2), 1 + random() % 256);
    for (size_t i = 0; i < 10000; i++) {
        Buffer buffer;
        for (size_t j = 0; j < 1100 && buffer.writeSize(); j++) {
            *buffer.writeData() = random();
            buffer.commit(1);
        }
        if (buffer.available()) {
            assert(buffer.available() <= 1024);
            uint8_t result[1024];
            assert(buffer.read(result, sizeof(result)) > 0);
        }
        buffer.reset();
        feed(buffer, {0xd0, 0});
        assert(buffer.available() == 2);
    }

    for (int port = 0; port < 512; ++port) {
        const auto type = static_cast<meshtastic_PortNum>(port);
        assert(MqttTextPolicy::accepts(type, 10, false, false, false) == (type == meshtastic_PortNum_TEXT_MESSAGE_APP));
        const bool broadcast = type == meshtastic_PortNum_TEXT_MESSAGE_APP || type == meshtastic_PortNum_NODEINFO_APP ||
                               type == meshtastic_PortNum_POSITION_APP;
        assert(MqttTextPolicy::accepts(type, 10, false, false, true) == broadcast);
        const bool essential = type == meshtastic_PortNum_TEXT_MESSAGE_APP || type == meshtastic_PortNum_ROUTING_APP ||
                               type == meshtastic_PortNum_NODEINFO_APP || type == meshtastic_PortNum_POSITION_APP;
        assert(MqttTextPolicy::accepts(type, 10, true, true, true) == essential);
        assert(MqttTextPolicy::accepts(type, 10, true, true, false) ==
               (type == meshtastic_PortNum_TEXT_MESSAGE_APP || type == meshtastic_PortNum_ROUTING_APP));
    }
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_TEXT_MESSAGE_APP, 0, false, false, true));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_ROUTING_APP, 10, true, false, true));
    assert(MqttTextPolicy::accepts(meshtastic_PortNum_NODEINFO_APP, 100, false, false, true));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_NODEINFO_APP, 100, false, false, false));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_NODEINFO_APP, 0, false, false, true));
    assert(MqttTextPolicy::accepts(meshtastic_PortNum_POSITION_APP, 30, false, false, true));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_POSITION_APP, 30, false, false, false));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_POSITION_APP, 0, false, false, true));
    assert(!MqttTextPolicy::accepts(meshtastic_PortNum_TELEMETRY_APP, 30, false, false, true));
    for (int i = 0; i < 10000; i++)
        assert(MqttTextPolicy::accepts(meshtastic_PortNum_TEXT_MESSAGE_APP, 10, false, false, i % 2));
    puts("PASS: fragmentation, QoS1, bounds, reset, 20000 valid frames, 10000 fuzz inputs, contacts toggle, no text quota");
}
