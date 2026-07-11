#pragma once

#include "Default.h"
#include "configuration.h"

#include "concurrency/OSThread.h"
#include "mesh/Channels.h"
#include "mesh/generated/meshtastic/mqtt.pb.h"
#if !defined(ARCH_NRF52) || NRF52_USE_JSON
#include "serialization/JSON.h"
#endif
#if HAS_WIFI
#include <WiFiClient.h>
#if __has_include(<WiFiClientSecure.h>)
#include <WiFiClientSecure.h>
#endif
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D) && !defined(ARCH_PORTDUINO)
class CrowPanelMqttClient : public WiFiClient
{
  public:
    size_t write(uint8_t data) override;
    size_t write(const uint8_t *buf, size_t size) override;
    using WiFiClient::write;
};
#endif
#endif
#if HAS_ETHERNET && !defined(USE_WS5500) && !defined(USE_CH390D)
#include <EthernetClient.h>
#endif

#if HAS_NETWORKING
#include <PubSubClient.h>
#include <memory>
#endif

#define MAX_MQTT_QUEUE 16

/**
 * Our wrapper/singleton for sending/receiving MQTT "udp" packets.  This object isolates the MQTT protocol implementation from
 * the two components that use it: MQTTPlugin and MQTTSimInterface.
 */
class MQTT : private concurrency::OSThread
{
  public:
    MQTT();

    /**
     * Publish a packet on the global MQTT server.
     * @param mp_encrypted the encrypted packet to publish
     * @param mp_decoded the decrypted packet to publish
     * @param chIndex the index of the channel for this message
     *
     * Note: for messages we are forwarding on the mesh that we can't find the channel for (because we don't have the keys), we
     * can not forward those messages to the cloud - because no way to find a global channel ID.
     */
    void onSend(const meshtastic_MeshPacket &mp_encrypted, const meshtastic_MeshPacket &mp_decoded, ChannelIndex chIndex);

    bool isConnectedDirectly();

    /** Open the direct broker connection immediately, on the calling thread.
     * Used on ESP32-P4 RGB-panel boards to establish the MQTT socket during the
     * clean-PSRAM window in setup() — BEFORE esp_lcd_new_rgb_panel() corrupts
     * the PSRAM TLSF heap. Without it the first connect happens from the MQTT
     * OSThread post-framebuffer and asserts in block_locate_free. No-op when
     * MQTT is disabled / proxied / no link is wanted. */
    void prewarmConnect();

    void pausePublicDownlink(uint32_t durationMs, const char *reason);

    bool publish(const char *topic, const char *payload, bool retained);

    bool publish(const char *topic, const uint8_t *payload, size_t length, const bool retained);

    void onClientProxyReceive(meshtastic_MqttClientProxyMessage msg);

    bool isEnabled() { return this->enabled; };

    void start() { setIntervalFromNow(0); };

    bool isUsingDefaultServer() { return isConfiguredForDefaultServer; }
    bool isUsingDefaultRootTopic() { return isConfiguredForDefaultRootTopic; }

    /// Validate the meshtastic_ModuleConfig_MQTTConfig.
    static bool isValidConfig(const meshtastic_ModuleConfig_MQTTConfig &config) { return isValidConfig(config, nullptr); }

  protected:
    struct QueueEntry {
        std::string topic;
        std::basic_string<uint8_t> envBytes; // binary/pb_encode_to_bytes ServiceEnvelope
    };
    PointerQueue<QueueEntry> mqttQueue;

    int reconnectCount = 0;
    bool isConfiguredForDefaultServer = true;
    bool isConfiguredForDefaultRootTopic = true;

    virtual int32_t runOnce() override;

#ifndef PIO_UNIT_TESTING
  private:
#endif
#if HAS_WIFI
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D) && !defined(ARCH_PORTDUINO)
    using MQTTClient = CrowPanelMqttClient;
#else
    using MQTTClient = WiFiClient;
#endif
#if __has_include(<WiFiClientSecure.h>)
    using MQTTClientTLS = WiFiClientSecure;
#define MQTT_SUPPORTS_TLS 1
#endif
#elif HAS_ETHERNET
    using MQTTClient = EthernetClient;
#else
    using MQTTClient = void;
#endif

#if HAS_NETWORKING
    std::unique_ptr<MQTTClient> mqttClient;
#if MQTT_SUPPORTS_TLS
    MQTTClientTLS mqttClientTLS;
#endif
    PubSubClient pubSub;
    explicit MQTT(std::unique_ptr<MQTTClient> mqttClient);
#endif

    std::string cryptTopic = "/2/e/";   // msh/2/e/CHANNELID/NODEID
    std::string jsonTopic = "/2/json/"; // msh/2/json/CHANNELID/NODEID
    std::string mapTopic = "/2/map/";   // For protobuf-encoded MapReport messages

    // For map reporting (only applies when enabled)
    const uint32_t default_map_position_precision = 14; // defaults to max. offset of ~1459m
    uint32_t last_report_to_map = 0;
    uint32_t map_position_precision = default_map_position_precision;
    uint32_t map_publish_interval_msecs = default_map_publish_interval_secs * 1000;

    /** Attempt to connect to server if necessary
     */
    void reconnect();

    /** Tell the server what subscriptions we want (based on channels.downlink_enabled)
     */
    void sendSubscriptions();

    /// Callback for direct mqtt subscription messages
    static void mqttCallback(char *topic, byte *payload, unsigned int length);

    static bool isValidConfig(const meshtastic_ModuleConfig_MQTTConfig &config, MQTTClient *client);

    /// Called when a new publish arrives from the MQTT server
    void onReceive(char *topic, byte *payload, size_t length);

    void publishQueuedMessages();

    void publishNodeInfo();

    // Check if we should report unencrypted information about our node for consumption by a map
    void perhapsReportToMap();

    /// Return 0 if sleep is okay, veto sleep if we are connected to pubsub server
    // int preflightSleepCb(void *unused = NULL) { return pubSub.connected() ? 1 : 0; }
};

void mqttInit();

extern MQTT *mqtt;
