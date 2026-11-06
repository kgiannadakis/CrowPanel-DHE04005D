#pragma once

#include <cstdint>
#include <string_view>

namespace mcui::node_search {

template <size_t N> std::string_view bounded_text(const char (&text)[N])
{
    size_t size = 0;
    while (size < N && text[size])
        ++size;
    return {text, size};
}

inline uint32_t next_codepoint(std::string_view text, size_t &offset)
{
    const auto first = static_cast<uint8_t>(text[offset++]);
    if (first < 0x80)
        return first;
    const size_t continuation = first >= 0xC2 && first <= 0xDF ? 1 :
                                first >= 0xE0 && first <= 0xEF ? 2 :
                                first >= 0xF0 && first <= 0xF4 ? 3 : 0;
    if (!continuation || continuation > text.size() - offset)
        return 0x110000 + first;
    uint32_t value = first & (0x7F >> (continuation + 1));
    for (size_t i = 0; i < continuation; ++i) {
        const auto byte = static_cast<uint8_t>(text[offset + i]);
        if ((byte & 0xC0) != 0x80)
            return 0x110000 + first;
        value = (value << 6) | (byte & 0x3F);
    }
    if ((continuation == 1 && value < 0x80) || (continuation == 2 && value < 0x800) ||
        (continuation == 3 && value < 0x10000) || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF)
        return 0x110000 + first;
    offset += continuation;
    return value;
}

// Simple case folding for the keyboard's Latin and Greek alphabets; accents are retained.
inline uint32_t fold(uint32_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE) ||
        (c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB))
        return c + 0x20;
    switch (c) {
    case 0x1E9E: return 0xDF;
    case 0x386: return 0x3AC;
    case 0x388: return 0x3AD;
    case 0x389: return 0x3AE;
    case 0x38A: return 0x3AF;
    case 0x38C: return 0x3CC;
    case 0x38E: return 0x3CD;
    case 0x38F: return 0x3CE;
    case 0x3C2: return 0x3C3;
    default: return c;
    }
}

inline bool contains(std::string_view name, std::string_view query)
{
    if (query.empty())
        return true;
    size_t start = 0;
    while (start < name.size()) {
        size_t candidate = start;
        size_t needle = 0;
        while (candidate < name.size() && needle < query.size()) {
            if (fold(next_codepoint(name, candidate)) != fold(next_codepoint(query, needle)))
                break;
            if (needle == query.size())
                return true;
        }
        next_codepoint(name, start);
    }
    return false;
}

} // namespace mcui::node_search
