#pragma once
#include <helpers/BaseChatMesh.h>
#include <helpers/SimpleMeshTables.h>

class MyMeshMinimal : public BaseChatMesh {
public:
  MyMeshMinimal(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc, SimpleMeshTables& tables)
  : BaseChatMesh(radio, rng, rtc, tables) {}

protected:
  // For now: just print any received text to Serial (and later to LVGL)
  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    (void)pkt; (void)ts;
    Serial.print("RX from ");
    Serial.print(from.name);
    Serial.print(": ");
    Serial.println(text ? text : "");
  }

  void onChannelMessageRecv(const mesh::GroupChannel& ch, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    (void)pkt; (void)ts;
    Serial.print("RX #");
    Serial.print(ch.name);
    Serial.print(": ");
    Serial.println(text ? text : "");
  }
};