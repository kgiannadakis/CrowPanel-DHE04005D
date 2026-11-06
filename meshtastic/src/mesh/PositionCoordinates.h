#pragma once

#include <cstdint>

namespace PositionCoordinates
{
inline bool isValid(int32_t latitude, int32_t longitude)
{
    return (latitude != 0 || longitude != 0) && latitude >= -900000000 && latitude <= 900000000 &&
           longitude >= -1800000000 && longitude <= 1800000000;
}

inline bool isComplete(bool hasLatitude, bool hasLongitude, int32_t latitude, int32_t longitude)
{
    return hasLatitude && hasLongitude && isValid(latitude, longitude);
}
} // namespace PositionCoordinates
