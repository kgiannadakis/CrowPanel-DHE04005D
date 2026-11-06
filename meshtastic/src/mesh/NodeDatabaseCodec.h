#pragma once

#include <vector>
#include <pb_decode.h>
#include <pb_encode.h>
#include "mesh/generated/meshtastic/deviceonly.pb.h"

inline bool nodeDatabaseFieldCodec(pb_istream_t *input, pb_ostream_t *output,
                                   const pb_field_iter_t *field, size_t limit)
{
    auto *nodes = static_cast<std::vector<meshtastic_NodeInfoLite> *>(field->pData);
    if (output) {
        for (const auto node : *nodes) {
            if (!node.num) continue;
            if (!pb_encode_tag_for_field(output, field) ||
                !pb_encode_submessage(output, meshtastic_NodeInfoLite_fields, &node))
                return false;
        }
    }
    if (input) {
        meshtastic_NodeInfoLite node = meshtastic_NodeInfoLite_init_zero;
        if (!pb_decode(input, meshtastic_NodeInfoLite_fields, &node)) return false;
        // Older saves include unused preallocated slots. They are not contacts.
        if (node.num && nodes->size() < limit) nodes->push_back(node);
    }
    return true;
}
