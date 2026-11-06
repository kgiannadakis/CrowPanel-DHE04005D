#include "CrowPanelStoragePolicy.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    std::vector<uint8_t> flash(0x230000, 0xFF);
    size_t readBytes = 0;
    bool failRead = false;
    auto read = [&](size_t offset, uint8_t *buffer, size_t length) {
        assert(offset + length <= flash.size());
        assert(length <= 512);
        if (failRead) return false;
        std::memcpy(buffer, flash.data() + offset, length);
        readBytes += length;
        return true;
    };
    assert(!crowpanel_storage::partitionIsErased(0, read));
    assert(readBytes == 0);
    assert(crowpanel_storage::partitionIsErased(flash.size(), read));
    assert(readBytes == flash.size());
    for (size_t position : {size_t(0), size_t(4096), size_t(65536),
                            size_t(1000000), flash.size() - 1}) {
        flash[position] = 0;
        assert(!crowpanel_storage::partitionIsErased(flash.size(), read));
        flash[position] = 0xFF;
    }
    failRead = true;
    assert(!crowpanel_storage::partitionIsErased(flash.size(), read));
    failRead = false;
    flash.resize(1025);
    assert(crowpanel_storage::partitionIsErased(flash.size(), read));
    flash.back() = 0xFE;
    assert(!crowpanel_storage::partitionIsErased(flash.size(), read));
    std::puts("PASS: blank-only storage initialization; every byte checked, nonblank/read failures refused");
}
