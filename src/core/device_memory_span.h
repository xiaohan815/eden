// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

#include "common/common_types.h"

namespace Core {

// Heap continuity is only a hint: device pages can be unmapped or remapped independently.
// Return the bytes in the first still-contiguous mapped run, or the current unmapped run.
template <size_t PageBits, typename Page>
size_t DeviceMemorySpanSize(std::span<const Page> pages, DAddr address, size_t size) {
    constexpr size_t page_size = size_t{1} << PageBits;
    constexpr size_t page_mask = page_size - 1;
    const size_t page_index = address >> PageBits;
    if (size == 0 || page_index >= pages.size()) {
        return size;
    }

    const size_t page_offset = address & page_mask;
    size_t available = std::min(size, page_size - page_offset);
    const u32 physical_page = pages[page_index].compressed_physical_ptr;
    if (physical_page == 0) {
        for (size_t i = 1; available < size && i < pages.size() - page_index; ++i) {
            if (pages[page_index + i].compressed_physical_ptr != 0) {
                break;
            }
            available += std::min(size - available, page_size);
        }
        return available;
    }

    const size_t max_pages =
        std::min(std::max(size_t{1}, size_t{pages[page_index].continuity_tracker}),
                 pages.size() - page_index);
    for (size_t i = 1; available < size && i < max_pages; ++i) {
        if (u64{pages[page_index + i].compressed_physical_ptr} != u64{physical_page} + i) {
            break;
        }
        available += std::min(size - available, page_size);
    }
    return available;
}

template <size_t PageBits, typename Page, typename OnUnmapped, typename OnMemory,
          typename Increment>
void WalkDeviceMemory(std::span<const Page> pages, DAddr address, size_t size,
                      OnUnmapped&& on_unmapped, OnMemory&& on_memory, Increment&& increment) {
    constexpr size_t page_mask = (size_t{1} << PageBits) - 1;
    while (size != 0) {
        const size_t copy_amount = DeviceMemorySpanSize<PageBits>(pages, address, size);
        const size_t page_index = address >> PageBits;
        const u32 physical_page =
            page_index < pages.size() ? pages[page_index].compressed_physical_ptr : 0;
        if (physical_page == 0) {
            on_unmapped(copy_amount, address);
        } else {
            on_memory(copy_amount, (PAddr{physical_page - 1} << PageBits) + (address & page_mask));
        }
        increment(copy_amount);
        address += copy_amount;
        size -= copy_amount;
    }
}

} // namespace Core
