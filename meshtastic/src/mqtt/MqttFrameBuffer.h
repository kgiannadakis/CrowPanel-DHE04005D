#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Expose complete frames to PubSubClient, without blocking on TCP fragments.
template <size_t Capacity> class MqttFrameBuffer
{
  public:
    uint8_t *writeData() { return storage + used; }
    size_t writeSize() const { return invalid || available() ? 0 : frameSize ? frameSize - used : 1; }
    bool partial() const { return used != 0 && available() == 0; }
    bool failed() const { return invalid; }
    size_t available() const { return frameSize && used == frameSize && !invalid ? frameSize - position : 0; }
    int peek() const { return available() ? storage[position] : -1; }

    void commit(size_t count)
    {
        if (count > writeSize()) {
            invalid = true;
            return;
        }
        used += count;
        if (count == 0 || used == 1)
            return;
        if (frameSize == 0) {
            const uint8_t digit = storage[used - 1];
            remaining += (digit & 127u) << (7 * lengthBytes++);
            if ((digit & 128u) != 0) {
                invalid = lengthBytes == 4;
                return;
            }
            frameSize = used + remaining;
            invalid = frameSize > Capacity;
        }
        if (!invalid && used == frameSize && (storage[0] & 0xf0) == 0x30) {
            const size_t offset = 1 + lengthBytes;
            if (remaining < 2) {
                invalid = true;
                return;
            }
            const size_t topicLength = (size_t(storage[offset]) << 8) | storage[offset + 1];
            const uint8_t qos = (storage[0] >> 1) & 3;
            invalid = topicLength == 0 || qos > 1 || remaining < 2 + topicLength + (qos ? 2 : 0);
        }
    }

    int read(uint8_t *dest, size_t count)
    {
        if (!dest)
            return -1;
        count = std::min(count, available());
        if (count == 0)
            return -1;
        memcpy(dest, storage + position, count);
        position += count;
        if (position == frameSize)
            reset();
        return static_cast<int>(count);
    }

    void reset()
    {
        used = position = frameSize = remaining = lengthBytes = 0;
        invalid = false;
    }

  private:
    static_assert(Capacity >= 5, "MQTT fixed header must fit");
    uint8_t storage[Capacity] = {};
    size_t used = 0;
    size_t position = 0;
    size_t frameSize = 0;
    uint32_t remaining = 0;
    uint8_t lengthBytes = 0;
    bool invalid = false;
};
