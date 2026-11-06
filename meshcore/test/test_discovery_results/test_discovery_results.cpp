#include <gtest/gtest.h>
#include "../../examples/crowpanel_p4_lvgl_chat/discovery_results.h"
#include "Packet.h"

TEST(DiscoveryResults, DuplicateRepliesUpdateMeasurementsWithoutAddingRows) {
    DiscoveryResults results;
    uint8_t key[32] = {1};
    results.record(key, "Repeater", -110.5f, -24, 12);
    results.record(key, "Renamed", -98.0f, 7, -3);
    DiscoveryResult row;
    ASSERT_EQ(1, results.snapshot(&row, 1));
    EXPECT_STREQ("Renamed", row.name);
    EXPECT_FLOAT_EQ(-98.0f, row.rssi);
    EXPECT_EQ(7, row.rx_snr_x4);
    EXPECT_EQ(-3, row.request_snr_x4);
    EXPECT_EQ(2, row.replies);
}

TEST(DiscoveryResults, UsesFullIdentityRatherThanShortPrefixes) {
    DiscoveryResults results;
    uint8_t first[32] = {1};
    uint8_t second[32] = {1};
    second[31] = 2;
    results.record(first, "First", -100.0f, 0, 0);
    results.record(second, "Second", -90.0f, 4, 8);
    DiscoveryResult rows[2];
    EXPECT_EQ(2, results.snapshot(rows, 2));
    ASSERT_TRUE(results.find(second, rows[0]));
    EXPECT_STREQ("Second", rows[0].name);
    EXPECT_FLOAT_EQ(-90.0f, rows[0].rssi);
}

TEST(DiscoveryResults, ClearRemovesPreviousScanAndResetsReplyCounts) {
    DiscoveryResults results;
    uint8_t key[32] = {1};
    results.record(key, "Repeater", -110.0f, 0, 0);
    results.clear();
    DiscoveryResult row;
    EXPECT_EQ(0, results.snapshot(&row, 1));
    EXPECT_FALSE(results.find(key, row));
    results.record(key, "Repeater", -80.0f, 8, 4);
    ASSERT_TRUE(results.find(key, row));
    EXPECT_EQ(1, row.replies);
    EXPECT_FLOAT_EQ(-80.0f, row.rssi);
}

TEST(DiscoveryResults, BoundsStorageSnapshotsAndNames) {
    DiscoveryResults results;
    uint8_t key[32] = {};
    for (int i = 0; i <= MAX_DISCOVER_RESULTS; ++i) {
        key[0] = i;
        results.record(key, "A repeater name longer than thirty one characters", -100.0f, 0, 0);
    }
    DiscoveryResult rows[MAX_DISCOVER_RESULTS];
    EXPECT_EQ(MAX_DISCOVER_RESULTS, results.snapshot(rows, MAX_DISCOVER_RESULTS));
    EXPECT_EQ(1, results.snapshot(rows, 1));
    EXPECT_EQ(31u, strlen(rows[0].name));
    EXPECT_EQ(0, results.snapshot(nullptr, 1));
    EXPECT_EQ(0, results.snapshot(rows, -1));
    EXPECT_FALSE(results.find(key, rows[0]));
    key[0] = 0;
    results.record(key, "Updated at capacity", -70.0f, 4, 8);
    ASSERT_TRUE(results.find(key, rows[0]));
    EXPECT_EQ(2, rows[0].replies);
    EXPECT_FLOAT_EQ(-70.0f, rows[0].rssi);
}

TEST(DiscoveryResults, ReplyCounterSaturates) {
    DiscoveryResults results;
    uint8_t key[32] = {1};
    for (unsigned i = 0; i <= UINT16_MAX; ++i) {
        results.record(key, "Repeater", -100.0f, 0, 0);
    }
    DiscoveryResult row;
    ASSERT_TRUE(results.find(key, row));
    EXPECT_EQ(UINT16_MAX, row.replies);
}

TEST(PacketSignalMetadata, DoesNotChangeWireFormat) {
    mesh::Packet packet;
    packet.header = ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_CONTROL << PH_TYPE_SHIFT);
    packet.payload_len = 1;
    packet.payload[0] = 0x92;
    uint8_t before[256], after[256];
    const uint8_t len = packet.writeTo(before);
    packet._rssi = -104.5f;
    packet._snr = -7;
    ASSERT_EQ(len, packet.writeTo(after));
    EXPECT_EQ(0, memcmp(before, after, len));
    mesh::Packet restored;
    ASSERT_TRUE(restored.readFrom(after, len));
    EXPECT_FLOAT_EQ(0.0f, restored._rssi);
    EXPECT_EQ(packet.header, restored.header);
    EXPECT_EQ(packet.payload[0], restored.payload[0]);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
