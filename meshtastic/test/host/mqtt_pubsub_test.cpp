#include "mqtt/MqttFrameBuffer.h"
#include <PubSubClient.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static unsigned long now = 1;
unsigned long millis() { return now; }
void yield() { ++now; }

class FramedClient : public Client
{
  public:
    int connect(IPAddress, uint16_t) override { return active = true; }
    int connect(const char *, uint16_t) override { return active = true; }
    uint8_t connected() override { return active; }
    void stop() override
    {
        active = false;
        frame.reset();
        incoming.clear();
        pos = released = 0;
    }
    void flush() override {}
    int available() override
    {
        if (frame.available())
            return frame.available();
        for (int i = 0; active && i < 8 && frame.writeSize() && pos < released; ++i) {
            const size_t size = std::min(frame.writeSize(), released - pos);
            memcpy(frame.writeData(), incoming.data() + pos, size);
            pos += size;
            frame.commit(size);
            if (frame.failed()) {
                stop();
                return 0;
            }
        }
        return frame.available();
    }
    int read() override
    {
        uint8_t value;
        return read(&value, 1) == 1 ? value : -1;
    }
    int read(uint8_t *data, size_t size) override { return frame.read(data, size); }
    int peek() override
    {
        available();
        return frame.peek();
    }
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *data, size_t size) override
    {
        outgoing.insert(outgoing.end(), data, data + size);
        if (size && (data[0] & 0xf0) == MQTTCONNECT)
            receive({MQTTCONNACK, 2, 0, 0});
        return size;
    }
    void receive(const std::vector<uint8_t> &data, bool complete = true)
    {
        incoming.insert(incoming.end(), data.begin(), data.end());
        if (complete)
            released = incoming.size();
    }
    void release(size_t count) { released = std::min(incoming.size(), released + count); }
    std::vector<uint8_t> outgoing;

  private:
    MqttFrameBuffer<1024> frame;
    bool active = false;
    std::vector<uint8_t> incoming;
    size_t pos = 0;
    size_t released = 0;
};

static std::vector<uint8_t> publish(const std::string &topic, const std::string &text, bool qos1)
{
    std::vector<uint8_t> packet{uint8_t(qos1 ? 0x32 : 0x30)};
    size_t remaining = 2 + topic.size() + text.size() + (qos1 ? 2 : 0);
    do {
        const uint8_t digit = remaining & 127;
        remaining >>= 7;
        packet.push_back(digit | (remaining ? 128 : 0));
    } while (remaining);
    packet.insert(packet.end(), {uint8_t(topic.size() >> 8), uint8_t(topic.size())});
    packet.insert(packet.end(), topic.begin(), topic.end());
    if (qos1)
        packet.insert(packet.end(), {0x12, 0x34});
    packet.insert(packet.end(), text.begin(), text.end());
    return packet;
}

int main()
{
    FramedClient transport;
    PubSubClient mqtt(transport);
    mqtt.setServer("test", 1883).setKeepAlive(30).setSocketTimeout(5);
    assert(mqtt.setBufferSize(1024, 1024));
    const std::string topic = "msh/EU_868/IT/2/e/MediumFast/!12345678";
    size_t callbacks = 0;
    std::string received;
    mqtt.setCallback([&](char *name, uint8_t *data, size_t size) {
        assert(name == topic);
        received.assign(reinterpret_cast<char *>(data), size);
        ++callbacks;
    });
    assert(mqtt.connect("host-regression-test"));
    assert(mqtt.subscribe("msh/EU_868/IT/2/e/MediumFast/+", 1));
    transport.receive({MQTTSUBACK, 3, 0, 2, 1});
    assert(mqtt.loop());

    for (bool qos1 : {false, true}) {
        const std::string text(200, qos1 ? 'q' : 't');
        const auto packet = publish(topic, text, qos1);
        transport.receive(packet, false);
        const size_t before = callbacks;
        const size_t written = transport.outgoing.size();
        const unsigned long started = now;
        for (size_t i = 0; i < packet.size(); ++i) {
            transport.release(1);
            assert(mqtt.loop());
            assert(callbacks == before + (i + 1 == packet.size()));
            assert(now == started); // Incomplete frames never enter readByte's wait loop.
        }
        assert(received == text);
        if (qos1) {
            const std::vector<uint8_t> ack(transport.outgoing.begin() + written, transport.outgoing.end());
            assert(ack == std::vector<uint8_t>({MQTTPUBACK, 2, 0x12, 0x34}));
        }
    }

    const size_t before = callbacks;
    for (int i = 0; i < 100; ++i)
        transport.receive(publish(topic, "chat", i % 2), false);
    transport.release(100000);
    for (int i = 0; i < 100; ++i)
        assert(mqtt.loop());
    assert(callbacks == before + 100);

    now += 31000;
    assert(mqtt.loop());
    transport.receive({MQTTPINGRESP, 0});
    assert(mqtt.loop());
    now += 31000;
    assert(mqtt.loop());
    transport.receive({MQTTPINGRESP, 0});
    assert(mqtt.loop());
    mqtt.disconnect();
    assert(!mqtt.connected());
    assert(mqtt.connect("host-regression-test"));
    transport.receive(publish(topic, "after reconnect", true));
    assert(mqtt.loop());
    assert(received == "after reconnect");
    puts("PASS: actual PubSubClient CONNECT, SUBACK, fragmented QoS0/1, PUBACK, 100-message burst, keepalive, reconnect");
}
