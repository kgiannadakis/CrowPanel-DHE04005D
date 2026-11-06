#include <Packet.h>
#include <string.h>

// The runner inserts the actual Dispatcher parser and Packet constructor here.
namespace mesh {
class Dispatcher {
public:
  bool tryParsePacket(Packet* pkt, const uint8_t* raw, int len);
};
// INSERT_PRODUCTION_METHODS
}

extern "C" void* memcpy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  const auto* s = static_cast<const uint8_t*>(src);
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
  return dst;
}

static bool equal(const uint8_t* a, const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return false;
  return true;
}

static uint8_t unhex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return c - 'a' + 10;
}

extern "C" uint32_t run_tests() {
  mesh::Dispatcher parser;
  mesh::Packet packet;
  uint8_t raw[256];
  const uint8_t payload[] = {0x11, 0x65, 0x9C, 0x8D, 0xCE, 0xCD, 0xD9};
  unsigned cases = 0;
  for (uint8_t route = 0; route < 4; ++route) {
    for (uint8_t width = 1; width <= 3; ++width) {
      // Exercise zero-hop and every legal hop count, including width boundaries.
      for (uint8_t hops = 0; hops <= 63 && hops * width <= MAX_PATH_SIZE; ++hops) {
        int pos = 0;
        raw[pos++] = (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT) | route;
        if (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT) {
          raw[pos++] = 0xFA; raw[pos++] = 0x28; raw[pos++] = 0; raw[pos++] = 0;
        }
        raw[pos++] = ((width - 1) << 6) | hops;
        const int path_start = pos;
        for (int i = 0; i < hops * width; ++i) raw[pos++] = static_cast<uint8_t>(i + 1);
        const int payload_start = pos;
        for (uint8_t b : payload) raw[pos++] = b;
        ++cases;
        if (!parser.tryParsePacket(&packet, raw, pos)) return 1000 + cases;
        if (packet.getPathHashSize() != width || packet.getPathHashCount() != hops ||
            packet.payload_len != sizeof(payload) || !equal(packet.payload, raw + payload_start, sizeof(payload)) ||
            !equal(packet.path, raw + path_start, hops * width)) return 2000 + cases;
        if (hops && parser.tryParsePacket(&packet, raw, payload_start - 1)) return 3000 + cases;
      }
    }
  }
  // Actual Public packets from 2026-10-31: DX jungle (2B), Domibe (#be + 2B), RIZZI (1B).
  const char* fixtures[] = {
    "1541A01B11659C8DCECDD977C1E259D6006FA34ED821352C61DE5288D93D27DCF4A8460D3B2C1317B6055937632210ED10FB4493CC4F2D",
    "14FA280000420123F2C911027990E0F26720F852896BA1AAB242409E7BD10EE81117254CE0BD58763E5A99FEABEA6861573DE16EB3F769D9876242FA19",
    "1502A30C11B0A612DEACDA022510F6121C70FCDA612E7306D65DB0554EC95B8B1BA1B244690D8C"
  };
  const uint8_t offsets[] = {4, 10, 4};
  const uint8_t widths[] = {2, 2, 1};
  for (unsigned f = 0; f < 3; ++f) {
    const char* text = fixtures[f];
    int len = 0;
    while (*text) { raw[len++] = (unhex(text[0]) << 4) | unhex(text[1]); text += 2; }
    if (!parser.tryParsePacket(&packet, raw, len) || packet.getPathHashSize() != widths[f] ||
        packet.payload_len != len - offsets[f] || packet.payload[0] != 0x11 ||
        !equal(packet.payload, raw + offsets[f], packet.payload_len)) return 4000 + f;
  }
  raw[0] = 0x15; raw[1] = 0xC1; raw[2] = 0;
  if (parser.tryParsePacket(&packet, raw, 3)) return 5001;  // reserved width
  raw[1] = 0x61;
  if (parser.tryParsePacket(&packet, raw, 70)) return 5002; // 33 * 2 > 64
  raw[0] = 0x55; raw[1] = 0;
  if (parser.tryParsePacket(&packet, raw, 3)) return 5003; // unsupported payload version, not path width
  return 0;
}
