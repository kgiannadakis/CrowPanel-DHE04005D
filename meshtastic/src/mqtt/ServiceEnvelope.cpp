#include "ServiceEnvelope.h"
#include <pb_decode.h>
#include <cstring>

namespace
{
bool readStringField(pb_istream_t *stream, char *dest, size_t destSize)
{
    if (!dest || destSize == 0 || stream->bytes_left >= destSize)
        return false;

    const size_t len = stream->bytes_left;
    if (!pb_read(stream, reinterpret_cast<pb_byte_t *>(dest), len))
        return false;

    dest[len] = '\0';
    return true;
}

bool decodeServiceEnvelopeNoMalloc(const uint8_t *payload, size_t length, DecodedServiceEnvelope *out)
{
    if (!payload || !out)
        return false;

    pb_istream_t stream = pb_istream_from_buffer(payload, length);
    bool sawPacket = false;
    bool sawChannel = false;
    bool sawGateway = false;

    while (stream.bytes_left > 0) {
        pb_wire_type_t wireType;
        uint32_t tag;
        bool eof = false;

        if (!pb_decode_tag(&stream, &wireType, &tag, &eof))
            return false;
        if (eof)
            break;

        if (tag == meshtastic_ServiceEnvelope_packet_tag) {
            if (wireType != PB_WT_STRING)
                return false;

            pb_istream_t substream;
            if (!pb_make_string_substream(&stream, &substream))
                return false;

            out->packetStorage = meshtastic_MeshPacket_init_zero;
            const bool ok = pb_decode(&substream, &meshtastic_MeshPacket_msg, &out->packetStorage);
            const bool fullyRead = substream.bytes_left == 0;
            if (!pb_close_string_substream(&stream, &substream))
                return false;
            if (!ok || !fullyRead)
                return false;

            out->packet = &out->packetStorage;
            sawPacket = true;
        } else if (tag == meshtastic_ServiceEnvelope_channel_id_tag) {
            if (wireType != PB_WT_STRING)
                return false;

            pb_istream_t substream;
            if (!pb_make_string_substream(&stream, &substream))
                return false;

            const bool ok = readStringField(&substream, out->channelStorage, sizeof(out->channelStorage));
            const bool fullyRead = substream.bytes_left == 0;
            if (!pb_close_string_substream(&stream, &substream))
                return false;
            if (!ok || !fullyRead)
                return false;

            out->channel_id = out->channelStorage;
            sawChannel = out->channelStorage[0] != '\0';
        } else if (tag == meshtastic_ServiceEnvelope_gateway_id_tag) {
            if (wireType != PB_WT_STRING)
                return false;

            pb_istream_t substream;
            if (!pb_make_string_substream(&stream, &substream))
                return false;

            const bool ok = readStringField(&substream, out->gatewayStorage, sizeof(out->gatewayStorage));
            const bool fullyRead = substream.bytes_left == 0;
            if (!pb_close_string_substream(&stream, &substream))
                return false;
            if (!ok || !fullyRead)
                return false;

            out->gateway_id = out->gatewayStorage;
            sawGateway = out->gatewayStorage[0] != '\0';
        } else if (!pb_skip_field(&stream, wireType)) {
            return false;
        }
    }

    return sawPacket && sawChannel && sawGateway;
}
} // namespace

DecodedServiceEnvelope::DecodedServiceEnvelope(const uint8_t *payload, size_t length)
{
    validDecode = decodeServiceEnvelopeNoMalloc(payload, length, this);
}

DecodedServiceEnvelope::DecodedServiceEnvelope(DecodedServiceEnvelope &&other)
{
    packetStorage = other.packetStorage;
    memcpy(channelStorage, other.channelStorage, sizeof(channelStorage));
    memcpy(gatewayStorage, other.gatewayStorage, sizeof(gatewayStorage));
    validDecode = other.validDecode;

    packet = validDecode && other.packet ? &packetStorage : nullptr;
    channel_id = validDecode && other.channel_id ? channelStorage : nullptr;
    gateway_id = validDecode && other.gateway_id ? gatewayStorage : nullptr;

    other.packet = nullptr;
    other.channel_id = nullptr;
    other.gateway_id = nullptr;
    other.validDecode = false;
}
