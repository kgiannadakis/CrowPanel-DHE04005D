#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using boolean = bool;
using byte = uint8_t;
using PGM_P = const char *;

unsigned long millis();
void yield();

#define PROGMEM
#define PSTR(text) text
#define pgm_read_byte_near(address) (*reinterpret_cast<const uint8_t *>(address))
#define strnlen_P strnlen

class Print
{
  public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *data, size_t size)
    {
        size_t written = 0;
        while (written < size && write(data[written]))
            ++written;
        return written;
    }
};
