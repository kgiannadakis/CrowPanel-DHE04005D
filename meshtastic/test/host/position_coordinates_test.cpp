#include "mesh/PositionCoordinates.h"

#include <cassert>
#include <climits>
#include <cstdio>

int main()
{
    using PositionCoordinates::isComplete;
    using PositionCoordinates::isValid;
    assert(isComplete(true, true, 432930816, 137232384));
    assert(isValid(-432930816, -137232384));
    assert(isComplete(true, true, 0, 137232384));
    assert(isComplete(true, true, 432930816, 0));
    assert(!isComplete(false, true, 432930816, 137232384));
    assert(!isComplete(true, false, 432930816, 137232384));
    assert(!isComplete(false, false, 0, 0));
    assert(!isComplete(true, true, 0, 0));
    assert(isValid(900000000, 1800000000));
    assert(isValid(-900000000, -1800000000));
    assert(!isValid(900000001, 1));
    assert(!isValid(-900000001, 1));
    assert(!isValid(1, 1800000001));
    assert(!isValid(1, -1800000001));
    assert(!isValid(INT32_MAX, INT32_MIN));
    assert(!isValid(INT32_MIN, INT32_MAX));
    std::puts("PASS: map coordinates, equator/prime meridian, missing fields, zero placeholders, signed range bounds");
}
