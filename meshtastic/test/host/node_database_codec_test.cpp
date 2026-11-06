#include "mesh/NodeDatabaseCodec.h"
#include "mesh/PositionCoordinates.h"
#include "mesh/generated/meshtastic/localonly.pb.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

static size_t decode_limit = 300;

extern "C" bool meshtastic_NodeDatabase_callback(pb_istream_t *input, pb_ostream_t *output, const pb_field_iter_t *field)
{
    return nodeDatabaseFieldCodec(input, output, field, decode_limit);
}

static std::vector<uint8_t> encode(const meshtastic_NodeDatabase &db)
{
    size_t size = 0;
    assert(pb_get_encoded_size(&size, meshtastic_NodeDatabase_fields, &db));
    std::vector<uint8_t> bytes(size);
    auto stream = pb_ostream_from_buffer(bytes.data(), bytes.size());
    assert(pb_encode(&stream, meshtastic_NodeDatabase_fields, &db));
    assert(stream.bytes_written == bytes.size());
    return bytes;
}

static bool decode(const uint8_t *bytes, size_t size, meshtastic_NodeDatabase &db)
{
    db.nodes.clear();
    db.version = 0;
    auto stream = pb_istream_from_buffer(bytes, size);
    return pb_decode(&stream, meshtastic_NodeDatabase_fields, &db);
}

int main()
{
    // First setup lives in separate config and device-state protobufs.
    meshtastic_LocalConfig setupConfig = meshtastic_LocalConfig_init_zero;
    setupConfig.version = 24;
    setupConfig.has_lora = true;
    setupConfig.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    meshtastic_DeviceState setupState = meshtastic_DeviceState_init_zero;
    setupState.version = 24;
    setupState.has_owner = true;
    std::strcpy(setupState.owner.long_name, "CrowPanel setup");
    std::strcpy(setupState.owner.short_name, "CP4");
    uint8_t setupBytes[meshtastic_LocalConfig_size + meshtastic_DeviceState_size] = {};
    auto setupOutput = pb_ostream_from_buffer(setupBytes, sizeof(setupBytes));
    assert(pb_encode(&setupOutput, meshtastic_LocalConfig_fields, &setupConfig));
    meshtastic_LocalConfig restoredConfig = meshtastic_LocalConfig_init_zero;
    auto setupInput = pb_istream_from_buffer(setupBytes, setupOutput.bytes_written);
    assert(pb_decode(&setupInput, meshtastic_LocalConfig_fields, &restoredConfig));
    assert(restoredConfig.has_lora && restoredConfig.version == 24);
    assert(restoredConfig.lora.region == meshtastic_Config_LoRaConfig_RegionCode_EU_868);
    setupOutput = pb_ostream_from_buffer(setupBytes, sizeof(setupBytes));
    assert(pb_encode(&setupOutput, meshtastic_DeviceState_fields, &setupState));
    meshtastic_DeviceState restoredState = meshtastic_DeviceState_init_zero;
    setupInput = pb_istream_from_buffer(setupBytes, setupOutput.bytes_written);
    assert(pb_decode(&setupInput, meshtastic_DeviceState_fields, &restoredState));
    assert(restoredState.has_owner && restoredState.version == 24);
    assert(std::strcmp(restoredState.owner.long_name, "CrowPanel setup") == 0);
    assert(std::strcmp(restoredState.owner.short_name, "CP4") == 0);

    // Exercise the same protobuf presence checks used by MQTT map admission.
    meshtastic_Position position = meshtastic_Position_init_zero;
    position.has_latitude_i = position.has_longitude_i = true;
    position.latitude_i = 0; // A valid zero latitude must still be present on the wire.
    position.longitude_i = 137232384;
    uint8_t positionBytes[128] = {};
    auto positionOutput = pb_ostream_from_buffer(positionBytes, sizeof(positionBytes));
    assert(pb_encode(&positionOutput, meshtastic_Position_fields, &position));
    meshtastic_Position decodedPosition = meshtastic_Position_init_zero;
    auto positionInput = pb_istream_from_buffer(positionBytes, positionOutput.bytes_written);
    assert(pb_decode(&positionInput, meshtastic_Position_fields, &decodedPosition));
    assert(PositionCoordinates::isComplete(decodedPosition.has_latitude_i, decodedPosition.has_longitude_i,
                                            decodedPosition.latitude_i, decodedPosition.longitude_i));
    position = meshtastic_Position_init_zero;
    position.time = 1791042895;
    positionOutput = pb_ostream_from_buffer(positionBytes, sizeof(positionBytes));
    assert(pb_encode(&positionOutput, meshtastic_Position_fields, &position));
    decodedPosition = meshtastic_Position_init_zero;
    positionInput = pb_istream_from_buffer(positionBytes, positionOutput.bytes_written);
    assert(pb_decode(&positionInput, meshtastic_Position_fields, &decodedPosition));
    assert(!PositionCoordinates::isComplete(decodedPosition.has_latitude_i, decodedPosition.has_longitude_i,
                                             decodedPosition.latitude_i, decodedPosition.longitude_i));
    const uint8_t truncatedPosition[] = {0x08, 0x80};
    positionInput = pb_istream_from_buffer(truncatedPosition, sizeof(truncatedPosition));
    decodedPosition = meshtastic_Position_init_zero;
    assert(!pb_decode(&positionInput, meshtastic_Position_fields, &decodedPosition));

    meshtastic_NodeDatabase db{};
    db.version = 24;
    db.nodes.resize(300);
    db.nodes[0].num = 0x0ae01942;
    db.nodes[1].num = 0xacaae48c; // Discovered by text, without NodeInfo yet.
    db.nodes[1].has_position = true;
    db.nodes[1].position.latitude_i = 432930816;
    db.nodes[1].position.longitude_i = 137232384;
    db.nodes[1].position.time = 1791042895;
    db.nodes[2].num = 0x12345678;
    db.nodes[2].has_user = true;
    db.nodes[2].via_mqtt = true;
    std::strcpy(db.nodes[2].user.long_name, "Heltec");
    db.nodes[2].user.public_key.size = 32;
    std::memset(db.nodes[2].user.public_key.bytes, 0x5a, 32);
    auto saved = encode(db);
    assert(saved.size() < 200); // Unused capacity is not serialized.
    meshtastic_NodeDatabase restored{};
    assert(decode(saved.data(), saved.size(), restored));
    assert(restored.nodes.size() == 3 && restored.version == 24);
    assert(restored.nodes[1].num == 0xacaae48c && !restored.nodes[1].has_user);
    assert(restored.nodes[1].has_position);
    assert(restored.nodes[1].position.latitude_i == 432930816);
    assert(restored.nodes[1].position.longitude_i == 137232384);
    assert(restored.nodes[1].position.time == 1791042895);
    assert(std::strcmp(restored.nodes[2].user.long_name, "Heltec") == 0);
    assert(restored.nodes[2].user.public_key.size == 32);
    assert(restored.nodes[2].via_mqtt);
    std::strcpy(db.nodes[2].user.long_name, "Heltec updated");
    db.nodes[1].position.latitude_i = 433000000;
    auto updated = encode(db);
    assert(decode(updated.data(), updated.size(), restored));
    assert(restored.nodes.size() == 3);
    assert(std::strcmp(restored.nodes[2].user.long_name, "Heltec updated") == 0);
    assert(restored.nodes[1].position.latitude_i == 433000000);
    assert(restored.nodes[1].position.longitude_i == 137232384);

    // A nested encode failure must reach the outer caller, not commit a prefix.
    for (size_t capacity = 0; capacity < saved.size(); ++capacity) {
        std::vector<uint8_t> short_buffer(capacity);
        auto stream = pb_ostream_from_buffer(short_buffer.data(), capacity);
        assert(!pb_encode(&stream, meshtastic_NodeDatabase_fields, &db));
    }
    const uint8_t malformed[] = {0x08, 0x18, 0x12, 0x01, 0x80};
    assert(!decode(malformed, sizeof(malformed), restored));
    auto partial = saved;
    partial.insert(partial.end(), {0x12, 0x01, 0x80});
    assert(!decode(partial.data(), partial.size(), restored));
    assert(restored.nodes.size() == 3);
    assert(decode(saved.data(), saved.size(), restored));
    assert(restored.nodes.size() == 3);
    const uint8_t old_empty_slots[] = {0x08, 0x18, 0x12, 0x00, 0x12, 0x00};
    assert(decode(old_empty_slots, sizeof(old_empty_slots), restored));
    assert(restored.nodes.empty());

    // Downgrading from the 1000-node build retains the first 300 without a decode failure.
    db.nodes.resize(1000);
    for (size_t i = 3; i < db.nodes.size(); ++i) {
        db.nodes[i] = db.nodes[2];
        std::memset(db.nodes[i].user.long_name, 'N', sizeof(db.nodes[i].user.long_name) - 1);
        std::memset(db.nodes[i].user.short_name, 'S', sizeof(db.nodes[i].user.short_name) - 1);
        db.nodes[i].num = 0x10000000 + i;
    }
    auto large = encode(db);
    assert(large.size() > 300 * meshtastic_NodeInfoLite_size);
    auto limitedInput = pb_istream_from_buffer(large.data(), 300 * meshtastic_NodeInfoLite_size);
    meshtastic_NodeDatabase limitedResult{};
    assert(!pb_decode(&limitedInput, meshtastic_NodeDatabase_fields, &limitedResult));
    assert(decode(large.data(), large.size(), restored));
    assert(restored.nodes.size() == 300);
    assert(restored.nodes.front().num == 0x0ae01942);
    assert(restored.nodes.back().num == db.nodes[299].num);
    assert(restored.nodes[1].has_position && restored.nodes[2].via_mqtt);
    decode_limit = 2;
    assert(decode(saved.data(), saved.size(), restored));
    assert(restored.nodes.size() == 2);

    // Backup recovery clears partial results before decoding the good snapshot.
    assert(!decode(malformed, sizeof(malformed), restored));
    decode_limit = 300;
    assert(decode(saved.data(), saved.size(), restored));
    assert(restored.nodes.size() == 3);
    assert(restored.nodes[1].has_position && restored.nodes[1].position.latitude_i == 432930816);
    std::puts("PASS: node/position round-trip, unnamed contacts, failed writes, malformed nodes, bounded decode, backup reload");
}
