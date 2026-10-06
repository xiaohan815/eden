// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/address_space.inc"

namespace {
using Allocator = Common::FlatAllocator<u32, 0, 8>;
}

TEST_CASE("Address allocator skips reservations at the linear cursor", "[address_allocator]") {
    Allocator allocator{16, 128};
    allocator.AllocateFixed(16, 32);
    REQUIRE(allocator.Allocate(16) == 48);
}

TEST_CASE("Address allocator skips reservations inside a requested range", "[address_allocator]") {
    Allocator allocator{16, 128};
    REQUIRE(allocator.Allocate(16) == 16);
    allocator.AllocateFixed(40, 40);
    REQUIRE(allocator.Allocate(16) == 80);
}

TEST_CASE("Address allocator reuses the first free range after exhaustion", "[address_allocator]") {
    Allocator allocator{16, 128};
    REQUIRE(allocator.Allocate(16) == 16);
    REQUIRE(allocator.Allocate(96) == 32);
    REQUIRE(allocator.Allocate(16) == 0);
    allocator.Free(16, 16);
    REQUIRE(allocator.Allocate(16) == 16);
}

TEST_CASE("Address allocator keeps forward allocation while space remains", "[address_allocator]") {
    Allocator allocator{16, 128};
    REQUIRE(allocator.Allocate(16) == 16);
    REQUIRE(allocator.Allocate(16) == 32);
    allocator.Free(16, 16);
    REQUIRE(allocator.Allocate(16) == 48);
}

TEST_CASE("Address allocator rejects invalid sizes without consuming space",
          "[address_allocator]") {
    Allocator allocator{16, 128};
    REQUIRE(allocator.Allocate(0) == 0);
    REQUIRE(allocator.Allocate(129) == 0);
    REQUIRE(allocator.Allocate(~u32{0}) == 0);
    REQUIRE(allocator.Allocate(16) == 16);
}

TEST_CASE("Address allocator does not wrap near the integer limit", "[address_allocator]") {
    Common::FlatAllocator<u64, 0, 64> allocator{~u64{0} - 31};
    REQUIRE(allocator.Allocate(16) == ~u64{0} - 31);
    REQUIRE(allocator.Allocate(16) == 0);
    allocator.Free(~u64{0} - 31, 16);
    REQUIRE(allocator.Allocate(16) == ~u64{0} - 31);
    REQUIRE(allocator.Allocate(15) == ~u64{0} - 15);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("Address allocator respects live ranges in a byte model", "[address_allocator]") {
    Allocator allocator{16, 128};
    std::array<bool, 128> used{};
    std::vector<std::pair<u32, u32>> live;
    std::mt19937 random{0xEDE0};
    for (size_t step = 0; step < 500; ++step) {
        if (!live.empty() && random() % 3 == 0) {
            const size_t index = random() % live.size();
            const auto [address, size] = live[index];
            allocator.Free(address, size);
            std::fill_n(used.begin() + address, size, false);
            live.erase(live.begin() + index);
            continue;
        }

        const u32 size = 1 + random() % 24;
        if (random() % 4 == 0) {
            const u32 address = 16 + random() % (128 - 16 - size + 1);
            if (std::none_of(used.begin() + address, used.begin() + address + size,
                             [](bool value) { return value; })) {
                allocator.AllocateFixed(address, size);
                std::fill_n(used.begin() + address, size, true);
                live.emplace_back(address, size);
            }
            continue;
        }

        const u32 address = allocator.Allocate(size);
        if (address == 0) {
            u32 free_run = 0;
            for (u32 byte = 16; byte < used.size(); ++byte) {
                free_run = used[byte] ? 0 : free_run + 1;
                REQUIRE(free_run < size);
            }
            continue;
        }
        REQUIRE(address >= 16);
        REQUIRE(address < used.size());
        REQUIRE(size <= used.size() - address);
        REQUIRE(std::none_of(used.begin() + address, used.begin() + address + size,
                             [](bool value) { return value; }));
        std::fill_n(used.begin() + address, size, true);
        live.emplace_back(address, size);
    }
}

TEST_CASE("Fixed allocation rejects occupied and invalid intervals", "[address_allocator]") {
    Allocator allocator{16, 128};
    REQUIRE(allocator.TryAllocateFixed(48, 16));
    CHECK_FALSE(allocator.TryAllocateFixed(48, 16));
    CHECK_FALSE(allocator.TryAllocateFixed(40, 32));
    CHECK_FALSE(allocator.TryAllocateFixed(56, 16));
    CHECK_FALSE(allocator.TryAllocateFixed(8, 16));
    CHECK_FALSE(allocator.TryAllocateFixed(128, 1));
    CHECK_FALSE(allocator.TryAllocateFixed(120, 16));
    CHECK_FALSE(allocator.TryAllocateFixed(32, 0));
    CHECK_FALSE(allocator.TryAllocateFixed(32, ~u32{0}));
    CHECK(allocator.Allocate(32) == 16);
    CHECK(allocator.Allocate(16) == 64);
    allocator.Free(48, 16);
    CHECK(allocator.TryAllocateFixed(48, 16));
}

TEST_CASE("Fixed allocations may touch existing allocations and the AS limit",
          "[address_allocator]") {
    Allocator allocator{16, 128};
    REQUIRE(allocator.TryAllocateFixed(48, 16));
    CHECK(allocator.TryAllocateFixed(16, 32));
    CHECK(allocator.TryAllocateFixed(64, 64));
    CHECK(allocator.Allocate(1) == 0);
    allocator.Free(64, 64);
    CHECK(allocator.Allocate(64) == 64);
}

TEST_CASE("Checked fixed allocations agree with an independent occupancy model",
          "[address_allocator]") {
    Allocator allocator{16, 128};
    std::array<bool, 128> used{};
    std::vector<std::pair<u32, u32>> live;
    std::mt19937 random{0xEDE04};
    for (size_t step = 0; step < 300; ++step) {
        if (!live.empty() && random() % 4 == 0) {
            const size_t index = random() % live.size();
            const auto [address, size] = live[index];
            allocator.Free(address, size);
            std::fill_n(used.begin() + address, size, false);
            live.erase(live.begin() + index);
            continue;
        }
        const u32 address = random() % 144;
        const u32 size = random() % 40;
        bool available =
            address >= 16 && address < used.size() && size != 0 && size <= used.size() - address;
        if (available) {
            available = std::none_of(used.begin() + address, used.begin() + address + size,
                                     [](bool value) { return value; });
        }
        INFO("step=" << step << " address=" << address << " size=" << size);
        CHECK(allocator.TryAllocateFixed(address, size) == available);
        if (available) {
            std::fill_n(used.begin() + address, size, true);
            live.emplace_back(address, size);
        }
    }
    for (const auto& [address, size] : live) {
        allocator.Free(address, size);
    }
    CHECK(allocator.Allocate(112) == 16);
    CHECK(allocator.Allocate(1) == 0);
}
