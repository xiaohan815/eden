// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/device_memory_span.h"

namespace {

constexpr size_t PAGE_BITS = 12;
constexpr size_t PAGE_SIZE = size_t{1} << PAGE_BITS;

struct Page {
    u32 continuity_tracker;
    u32 compressed_physical_ptr;
};

struct Memory {
    Memory() : backing(4 * PAGE_SIZE) {
        for (size_t i = 0; i < 4; ++i) {
            std::fill_n(backing.begin() + i * PAGE_SIZE, PAGE_SIZE, u8{0x11} * (i + 1));
        }
    }

    std::vector<u8> Read(DAddr address, size_t size) const {
        std::vector<u8> result(size, 0xFF);
        size_t offset = 0;
        Core::WalkDeviceMemory<PAGE_BITS>(
            std::span<const Page>{pages}, address, size,
            [&](size_t amount, DAddr) { std::memset(result.data() + offset, 0, amount); },
            [&](size_t amount, PAddr physical) {
                std::memcpy(result.data() + offset, backing.data() + physical, amount);
            },
            [&](size_t amount) { offset += amount; });
        return result;
    }

    void Write(DAddr address, std::span<const u8> data) {
        size_t offset = 0;
        Core::WalkDeviceMemory<PAGE_BITS>(
            std::span<const Page>{pages}, address, data.size(), [](size_t, DAddr) {},
            [&](size_t amount, PAddr physical) {
                std::memcpy(backing.data() + physical, data.data() + offset, amount);
            },
            [&](size_t amount) { offset += amount; });
    }

    // Continuity describes the original CPU heap, before its middle device page was unmapped.
    std::array<Page, 3> pages{{{3, 1}, {2, 0}, {1, 3}}};
    std::vector<u8> backing;
};

} // namespace

TEST_CASE("Device memory reads stop at holes in a contiguous CPU heap", "[device_memory]") {
    Memory memory;
    std::vector<u8> expected(3 * PAGE_SIZE);
    std::fill_n(expected.begin(), PAGE_SIZE, 0x11);
    std::fill_n(expected.begin() + 2 * PAGE_SIZE, PAGE_SIZE, 0x33);
    REQUIRE(memory.Read(0, expected.size()) == expected);

    // Starting inside an unmapped page must not hide a mapped page after it.
    expected.erase(expected.begin(), expected.begin() + PAGE_SIZE + 123);
    REQUIRE(memory.Read(PAGE_SIZE + 123, expected.size()) == expected);
}

TEST_CASE("Device memory remapping invalidates physical continuity", "[device_memory]") {
    Memory memory;
    memory.pages[1].compressed_physical_ptr = 4;
    std::vector<u8> expected(3 * PAGE_SIZE);
    std::fill_n(expected.begin(), PAGE_SIZE, 0x11);
    std::fill_n(expected.begin() + PAGE_SIZE, PAGE_SIZE, 0x44);
    std::fill_n(expected.begin() + 2 * PAGE_SIZE, PAGE_SIZE, 0x33);
    REQUIRE(memory.Read(0, expected.size()) == expected);

    expected.erase(expected.begin(), expected.begin() + PAGE_SIZE - 7);
    expected.resize(PAGE_SIZE + 14);
    REQUIRE(memory.Read(PAGE_SIZE - 7, expected.size()) == expected);
}

TEST_CASE("Device memory spans require every page to stay mapped", "[device_memory]") {
    Memory memory;
    const std::span<const Page> pages{memory.pages};
    REQUIRE(Core::DeviceMemorySpanSize<PAGE_BITS>(pages, 0, 3 * PAGE_SIZE) == PAGE_SIZE);
    REQUIRE(Core::DeviceMemorySpanSize<PAGE_BITS>(pages, PAGE_SIZE - 9, 18) == 9);
    memory.pages[1].compressed_physical_ptr = 4;
    REQUIRE(Core::DeviceMemorySpanSize<PAGE_BITS>(pages, 0, 3 * PAGE_SIZE) == PAGE_SIZE);
    memory.pages[1].compressed_physical_ptr = 2;
    REQUIRE(Core::DeviceMemorySpanSize<PAGE_BITS>(pages, 0, 3 * PAGE_SIZE) == 3 * PAGE_SIZE);
    REQUIRE(Core::DeviceMemorySpanSize<PAGE_BITS>(pages, PAGE_SIZE - 9, 18) == 18);
}

TEST_CASE("Device memory writes skip holes without touching their old backing", "[device_memory]") {
    Memory memory;
    const std::vector<u8> data(3 * PAGE_SIZE, 0x55);
    memory.Write(0, data);
    std::vector<u8> expected(4 * PAGE_SIZE, 0x55);
    std::fill_n(expected.begin() + PAGE_SIZE, PAGE_SIZE, 0x22);
    std::fill_n(expected.begin() + 3 * PAGE_SIZE, PAGE_SIZE, 0x44);
    REQUIRE(memory.backing == expected);

    Memory remapped;
    remapped.pages[1].compressed_physical_ptr = 4;
    remapped.Write(0, data);
    expected.assign(4 * PAGE_SIZE, 0x55);
    std::fill_n(expected.begin() + PAGE_SIZE, PAGE_SIZE, 0x22);
    REQUIRE(remapped.backing == expected);
}

TEST_CASE("Device memory handles an unmapped leading page and partial writes", "[device_memory]") {
    Memory memory;
    memory.pages[0].compressed_physical_ptr = 0;
    std::vector<u8> expected(3 * PAGE_SIZE);
    std::fill_n(expected.begin() + 2 * PAGE_SIZE, PAGE_SIZE, 0x33);
    REQUIRE(memory.Read(0, expected.size()) == expected);

    const std::vector<u8> data(PAGE_SIZE + 14, 0x55);
    memory.Write(PAGE_SIZE - 7, data);
    REQUIRE(std::all_of(memory.backing.begin(), memory.backing.begin() + PAGE_SIZE,
                        [](u8 byte) { return byte == 0x11; }));
    REQUIRE(std::all_of(memory.backing.begin() + PAGE_SIZE, memory.backing.begin() + 2 * PAGE_SIZE,
                        [](u8 byte) { return byte == 0x22; }));
    REQUIRE(std::all_of(memory.backing.begin() + 2 * PAGE_SIZE,
                        memory.backing.begin() + 2 * PAGE_SIZE + 7,
                        [](u8 byte) { return byte == 0x55; }));
    REQUIRE(memory.backing[2 * PAGE_SIZE + 7] == 0x33);
}

TEST_CASE("Device memory limits stale hints to the address space", "[device_memory]") {
    Memory memory;
    memory.pages[1] = Page{0, 2};
    memory.pages[2].continuity_tracker = 100;
    REQUIRE(memory.Read(PAGE_SIZE, PAGE_SIZE) == std::vector<u8>(PAGE_SIZE, 0x22));
    std::vector<u8> expected(PAGE_SIZE + 17, 0);
    std::fill_n(expected.begin(), PAGE_SIZE, 0x33);
    REQUIRE(memory.Read(2 * PAGE_SIZE, expected.size()) == expected);
    REQUIRE(memory.Read(3 * PAGE_SIZE, 17) == std::vector<u8>(17, 0));
    REQUIRE(memory.Read(0, 0).empty());
    memory.Write(0, {});
}

TEST_CASE("A 64 KiB device read retains mapped pages beyond a hole", "[device_memory]") {
    std::array<Page, 16> pages;
    for (u32 i = 0; i < pages.size(); ++i) {
        pages[i] = Page{16 - i, 32 + i};
    }
    pages[1].compressed_physical_ptr = 0;
    std::vector<std::pair<DAddr, size_t>> holes;
    std::vector<std::pair<PAddr, size_t>> mapped;
    size_t consumed = 0;
    Core::WalkDeviceMemory<PAGE_BITS>(
        std::span<const Page>{pages}, 0, 64 * 1024,
        [&](size_t size, DAddr address) { holes.emplace_back(address, size); },
        [&](size_t size, PAddr address) { mapped.emplace_back(address, size); },
        [&](size_t size) { consumed += size; });
    REQUIRE(holes == std::vector<std::pair<DAddr, size_t>>{{PAGE_SIZE, PAGE_SIZE}});
    REQUIRE(mapped == std::vector<std::pair<PAddr, size_t>>{{31 * PAGE_SIZE, PAGE_SIZE},
                                                            {33 * PAGE_SIZE, 14 * PAGE_SIZE}});
    REQUIRE(consumed == 64 * 1024);
}

TEST_CASE("Device memory chunking agrees with individual page mappings", "[device_memory]") {
    // Small pages keep this exhaustive byte comparison inexpensive.
    constexpr size_t bits = 4;
    constexpr size_t page_size = size_t{1} << bits;
    std::array<Page, 32> pages;
    std::array<u8, 64 * page_size> backing;
    std::mt19937 random{0xEDE01234};
    for (auto& byte : backing) {
        byte = static_cast<u8>(random());
    }
    for (size_t trial = 0; trial < 500; ++trial) {
        for (size_t i = 0; i < pages.size(); ++i) {
            const u32 physical = (trial & 1) ? static_cast<u32>(i + 1) : random() % 65;
            pages[i] = Page{static_cast<u32>(random()), physical};
            if ((random() & 7) == 0) {
                pages[i].compressed_physical_ptr = 0;
            }
        }
        const size_t address = random() % (pages.size() * page_size + 16);
        const size_t size = random() % (pages.size() * page_size + 16);
        std::vector<u8> expected(size, 0);
        for (size_t i = 0; i < size; ++i) {
            const size_t page = (address + i) / page_size;
            if (page < pages.size() && pages[page].compressed_physical_ptr != 0) {
                const size_t physical = (pages[page].compressed_physical_ptr - 1) * page_size;
                expected[i] = backing[physical + (address + i) % page_size];
            }
        }
        std::vector<u8> actual(size, 0xFF);
        size_t offset = 0;
        Core::WalkDeviceMemory<bits>(
            std::span<const Page>{pages}, address, size,
            [&](size_t amount, DAddr) { std::memset(actual.data() + offset, 0, amount); },
            [&](size_t amount, PAddr physical) {
                std::memcpy(actual.data() + offset, backing.data() + physical, amount);
            },
            [&](size_t amount) { offset += amount; });
        REQUIRE(actual == expected);
        REQUIRE(offset == size);
    }
}
