#pragma once

#include <stddef.h>
#include <stdint.h>

namespace crowpanel_storage {
template <typename Read>
bool partitionIsErased(size_t size, Read read)
{
    if (!size) return false;
    uint8_t buffer[512];
    for (size_t offset = 0; offset < size;) {
        const size_t length = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer);
        if (!read(offset, buffer, length)) return false;
        for (size_t i = 0; i < length; ++i)
            if (buffer[i] != 0xFF) return false;
        offset += length;
    }
    return true;
}
} // namespace crowpanel_storage
