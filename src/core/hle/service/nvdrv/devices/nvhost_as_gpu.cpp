// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace Service::Nvidia::Devices {

nvhost_as_gpu::nvhost_as_gpu(Core::System& system_, Module& module_, NvCore::Container& core)
    : nvdevice{system_}, module{module_}, container{core}, nvmap{core.GetNvMapFile()}, vm{},
      gmmu{} {}

nvhost_as_gpu::~nvhost_as_gpu() = default;

NvResult nvhost_as_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<u8> output) {
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_as_gpu::BindChannel, input, output);
        case 0x2:
            return WrapFixed(this, &nvhost_as_gpu::AllocateSpace, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_as_gpu::FreeSpace, input, output);
        case 0x5:
            return WrapFixed(this, &nvhost_as_gpu::UnmapBuffer, input, output);
        case 0x6:
            return WrapFixed(this, &nvhost_as_gpu::MapBufferEx, input, output);
        case 0x8:
            return WrapFixed(this, &nvhost_as_gpu::GetVARegions1, input, output);
        case 0x9:
            return WrapFixed(this, &nvhost_as_gpu::AllocAsEx, input, output);
        case 0x14:
            return WrapVariable(this, &nvhost_as_gpu::Remap, input, output);
        default:
            break;
        }
        break;
    default:
        break;
    }

    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<u8> output, std::span<u8> inline_output) {
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x8:
            return WrapFixedInlOut(this, &nvhost_as_gpu::GetVARegions3, input, output,
                                   inline_output);
        default:
            break;
        }
        break;
    default:
        break;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvhost_as_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {}
void nvhost_as_gpu::OnClose(DeviceFD fd) {}

NvResult nvhost_as_gpu::AllocAsEx(IoctlAllocAsEx& params) {
    LOG_DEBUG(Service_NVDRV, "called, big_page_size={:#X}", params.big_page_size);

    std::scoped_lock lock(mutex);

    if (vm.initialised) {
        ASSERT_MSG(false, "Cannot initialise an address space twice!");
        return NvResult::InvalidState;
    }

    const u32 big_page_size =
        params.big_page_size ? static_cast<u32>(params.big_page_size) : VM::DEFAULT_BIG_PAGE_SIZE;
    if (!std::has_single_bit(big_page_size) ||
        (big_page_size & VM::SUPPORTED_BIG_PAGE_SIZES) == 0) {
        LOG_ERROR(Service_NVDRV, "Unsupported big page size: {:#X}", big_page_size);
        return NvResult::BadValue;
    }
    const u64 range_start = params.va_range_start ? static_cast<u64>(params.va_range_start)
                                                  : u64{big_page_size} << VM::VA_START_SHIFT;
    const u64 range_split =
        params.va_range_start ? static_cast<u64>(params.va_range_split) : VM::DEFAULT_VA_SPLIT;
    const u64 range_end =
        params.va_range_start ? static_cast<u64>(params.va_range_end) : VM::DEFAULT_VA_RANGE;
    // Maxwell GPU virtual addresses are at most 40 bits. Validate before
    // narrowing page indices or constructing the page tables and allocators.
    if (range_start == 0 || range_start >= range_split || range_split >= range_end ||
        range_end > (u64{1} << 40) || !Common::IsAligned(range_start, VM::YUZU_PAGESIZE) ||
        !Common::IsAligned(range_split, big_page_size) ||
        !Common::IsAligned(range_end, big_page_size)) {
        return NvResult::BadValue;
    }
    vm.big_page_size = big_page_size;
    vm.big_page_size_bits = static_cast<u32>(std::countr_zero(big_page_size));
    vm.va_range_start = range_start;
    vm.va_range_split = range_split;
    vm.va_range_end = range_end;

    const u64 max_big_page_bits = Common::Log2Ceil64(vm.va_range_end);

    const auto start_pages{static_cast<u32>(vm.va_range_start >> VM::PAGE_SIZE_BITS)};
    const auto end_pages{static_cast<u32>(vm.va_range_split >> VM::PAGE_SIZE_BITS)};
    vm.small_page_allocator.emplace(start_pages, end_pages);

    const auto start_big_pages{static_cast<u32>(vm.va_range_split >> vm.big_page_size_bits)};
    const auto end_big_pages{static_cast<u32>(vm.va_range_end >> vm.big_page_size_bits)};
    vm.big_page_allocator.emplace(start_big_pages, end_big_pages);

    gmmu = std::make_shared<Tegra::MemoryManager>(system, max_big_page_bits, vm.va_range_split,
                                                  vm.big_page_size_bits, VM::PAGE_SIZE_BITS);
    system.GPU().InitAddressSpace(*gmmu);
    vm.initialised = true;

    return NvResult::Success;
}

NvResult nvhost_as_gpu::AllocateSpace(IoctlAllocSpace& params) {
    LOG_DEBUG(Service_NVDRV, "called, pages={:X}, page_size={:X}, flags={:X}", params.pages,
              params.page_size, params.flags);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    if (params.page_size != VM::YUZU_PAGESIZE && params.page_size != vm.big_page_size) {
        return NvResult::BadValue;
    }

    if (params.page_size != vm.big_page_size &&
        ((params.flags & MappingFlags::Sparse) != MappingFlags::None)) {
        UNIMPLEMENTED_MSG("Sparse small pages are not implemented!");
        return NvResult::NotImplemented;
    }

    const u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE ? VM::PAGE_SIZE_BITS
                                                                   : vm.big_page_size_bits};

    auto& allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator
                                                          : *vm.big_page_allocator};

    if (params.pages == 0) {
        return NvResult::BadValue;
    }
    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None) {
        const u64 offset = params.offset;
        const u64 first = u64{allocator.GetVAStart()} << page_size_bits;
        const u64 end = u64{allocator.GetVALimit()} << page_size_bits;
        const u64 size = u64{params.pages} * params.page_size;
        if (!Common::IsAligned(offset, params.page_size) || offset < first || offset >= end ||
            size > end - offset) {
            return NvResult::BadValue;
        }
        if (!allocator.TryAllocateFixed(static_cast<u32>(offset >> page_size_bits), params.pages)) {
            return NvResult::AlreadyAllocated;
        }
    } else {
        params.offset = static_cast<u64>(allocator.Allocate(params.pages)) << page_size_bits;
        if (!params.offset) {
            return NvResult::InsufficientMemory;
        }
    }

    u64 size{static_cast<u64>(params.pages) * params.page_size};

    if ((params.flags & MappingFlags::Sparse) != MappingFlags::None) {
        gmmu->MapSparse(params.offset, size);
    }

    allocation_map[params.offset] = {
        .mappings{},
        .size = size,
        .page_size = params.page_size,
        .sparse = (params.flags & MappingFlags::Sparse) != MappingFlags::None,
        .big_pages = params.page_size != VM::YUZU_PAGESIZE,
    };

    return NvResult::Success;
}

bool nvhost_as_gpu::FreeMappingLocked(u64 offset) noexcept {
    if (auto const it = mapping_map.find(offset); it != mapping_map.end()) {
        auto const mapping = it->second;
        if (!mapping.fixed) {
            auto& allocator{mapping.big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
            u32 page_size_bits{mapping.big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};
            u32 page_size{mapping.big_page ? vm.big_page_size : VM::YUZU_PAGESIZE};
            u64 aligned_size{Common::AlignUp(mapping.size, page_size)};
            allocator.Free(u32(mapping.offset >> page_size_bits),
                           u32(aligned_size >> page_size_bits));
        }
        // Invalidate the old mapping before releasing its pins, then restore
        // the reserved sparse state. Only FreeSpace releases the reservation.
        gmmu->Unmap(offset, mapping.size);
        if (mapping.sparse_alloc) {
            gmmu->MapSparse(offset, mapping.size, mapping.big_page);
        }
        ReleaseRemapPinsLocked(offset, mapping.size);
        nvmap.UnpinHandle(mapping.handle);
        if (mapping.fixed) {
            auto allocation = allocation_map.upper_bound(offset);
            if (allocation != allocation_map.begin()) {
                --allocation;
                std::erase(allocation->second.mappings, offset);
            }
        }
        map_buffer_offsets.erase(static_cast<s64>(offset));
        mapping_map.erase(offset);
        return true;
    }
    return false;
}

void nvhost_as_gpu::ReleaseRemapPinsLocked(u64 offset, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 end = offset + size;
    auto it = remapped_ranges.upper_bound(offset);
    if (it != remapped_ranges.begin()) {
        const auto previous = std::prev(it);
        if (previous->second.size > offset - previous->first) {
            it = previous;
        }
    }
    while (it != remapped_ranges.end() && it->first < end) {
        auto node = remapped_ranges.extract(it++);
        const u64 start = node.key();
        const u64 range_end = start + node.mapped().size;
        auto pin = std::move(node.mapped().pin);
        if (start < offset) {
            remapped_ranges.emplace(start, RemappedRange{offset - start, pin});
        }
        if (range_end > end) {
            remapped_ranges.emplace(end, RemappedRange{range_end - end, pin});
        }
        if (pin.use_count() == 1) {
            nvmap.UnpinHandle(*pin);
        }
    }
}

NvResult nvhost_as_gpu::FreeSpace(IoctlFreeSpace& params) {
    LOG_DEBUG(Service_NVDRV, "called, offset={:X}, pages={:X}, page_size={:X}", params.offset,
              params.pages, params.page_size);
    std::scoped_lock lock(mutex);
    if (!vm.initialised) {
        return NvResult::BadValue;
    }
    if (auto const it = allocation_map.find(params.offset); it != allocation_map.end()) {
        auto const allocation = it->second;
        if (allocation.page_size != params.page_size || allocation.size != (u64(params.pages) * params.page_size))
            return NvResult::BadValue;

        for (const auto mapping_offset : allocation.mappings)
            if (!FreeMappingLocked(mapping_offset))
                return NvResult::BadValue;

        // Unset sparse flag if required
        if (allocation.sparse)
            gmmu->Unmap(params.offset, allocation.size);
        ReleaseRemapPinsLocked(params.offset, allocation.size);

        auto& allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator : *vm.big_page_allocator};
        u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE ? VM::PAGE_SIZE_BITS : vm.big_page_size_bits};

        allocator.Free(u32(params.offset >> page_size_bits), u32(allocation.size >> page_size_bits));
        allocation_map.erase(params.offset);
        return NvResult::Success;
    }
    return NvResult::BadValue;
}

NvResult nvhost_as_gpu::Remap(std::span<IoctlRemapEntry> entries) {
    LOG_DEBUG(Service_NVDRV, "called, num_entries={:#X}", entries.size());

    std::scoped_lock lock(mutex);
    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    for (const auto& entry : entries) {
        GPUVAddr virtual_address{static_cast<u64>(entry.as_offset_big_pages)
                                 << vm.big_page_size_bits};
        u64 size{static_cast<u64>(entry.big_pages) << vm.big_page_size_bits};

        auto alloc{allocation_map.upper_bound(virtual_address)};

        if (size == 0 || alloc == allocation_map.begin()) {
            LOG_WARNING(Service_NVDRV, "Cannot remap into an unallocated region!");
            return NvResult::BadValue;
        }
        --alloc;
        const u64 relative_offset = virtual_address - alloc->first;
        if (relative_offset > alloc->second.size || size > alloc->second.size - relative_offset) {
            LOG_WARNING(Service_NVDRV, "Cannot remap beyond the reserved GPU region");
            return NvResult::BadValue;
        }

        if (!alloc->second.sparse) {
            LOG_WARNING(Service_NVDRV, "Cannot remap a non-sparse mapping!");
            return NvResult::BadValue;
        }

        const bool use_big_pages = alloc->second.big_pages;
        if (!entry.handle) {
            gmmu->Unmap(virtual_address, size);
            gmmu->MapSparse(virtual_address, size, use_big_pages);
            ReleaseRemapPinsLocked(virtual_address, size);
        } else {
            auto handle{nvmap.GetHandle(entry.handle)};
            const u64 buffer_offset = static_cast<u64>(entry.handle_offset_big_pages)
                                      << vm.big_page_size_bits;
            if (!handle || !handle->allocated || buffer_offset > handle->aligned_size ||
                size > handle->aligned_size - buffer_offset) {
                return NvResult::BadValue;
            }

            const DAddr base = nvmap.PinHandle(entry.handle, false);
            if (base == 0) {
                return NvResult::InsufficientMemory;
            }
            auto pin_guard = SCOPE_GUARD {
                nvmap.UnpinHandle(entry.handle);
            };
            auto pin = std::make_shared<NvCore::NvMap::Handle::Id>(entry.handle);
            const DAddr device_address = base + buffer_offset;

            gmmu->Unmap(virtual_address, size);
            gmmu->Map(virtual_address, device_address, size,
                      static_cast<Tegra::PTEKind>(entry.kind), use_big_pages);
            ReleaseRemapPinsLocked(virtual_address, size);
            remapped_ranges.emplace(virtual_address, RemappedRange{size, std::move(pin)});
            pin_guard.Cancel();
        }
    }

    return NvResult::Success;
}

NvResult nvhost_as_gpu::MapBufferEx(IoctlMapBufferEx& params) {
    LOG_DEBUG(Service_NVDRV,
              "called, flags={:X}, nvmap_handle={:X}, buffer_offset={}, mapping_size={}"
              ", offset={:#X}",
              params.flags, params.handle, params.buffer_offset, params.mapping_size,
              params.offset);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    // Remaps a subregion of an existing mapping to a different PA
    if ((params.flags & MappingFlags::Remap) != MappingFlags::None) {
        if (auto const it = mapping_map.find(params.offset); it != mapping_map.end()) {
            auto const mapping = it->second;
            const s64 buffer_offset = params.buffer_offset;
            const u64 size = params.mapping_size;
            if (buffer_offset < 0 || size == 0 || static_cast<u64>(buffer_offset) > mapping.size ||
                size > mapping.size - static_cast<u64>(buffer_offset)) {
                LOG_WARNING(Service_NVDRV,
                            "Cannot remap a partially mapped GPU address space region: {:#X}",
                            params.offset);
                return NvResult::BadValue;
            }
            const u64 relative_offset = static_cast<u64>(buffer_offset);
            const u64 mapped_size = Common::AlignUp(size, VM::YUZU_PAGESIZE);
            if (!Common::IsAligned(relative_offset, VM::YUZU_PAGESIZE) ||
                mapped_size > mapping.size - relative_offset) {
                LOG_WARNING(Service_NVDRV,
                            "Cannot remap an unaligned or out-of-bounds GPU subregion: {:#X}",
                            params.offset);
                return NvResult::BadValue;
            }
            const GPUVAddr gpu_address = mapping.offset + relative_offset;
            const DAddr device_address = mapping.ptr + relative_offset;
            const bool use_big_pages = mapping.big_page &&
                                       Common::IsAligned(gpu_address, vm.big_page_size) &&
                                       Common::IsAligned(mapped_size, vm.big_page_size);
            gmmu->Map(gpu_address, device_address, mapped_size, Tegra::PTEKind(params.kind),
                      use_big_pages);
            ReleaseRemapPinsLocked(gpu_address, mapped_size);
            return NvResult::Success;
        } else {
            LOG_WARNING(Service_NVDRV, "Cannot remap an unmapped GPU address space region: {:#X}",
                        params.offset);
            return NvResult::BadValue;
        }
    }

    auto handle{nvmap.GetHandle(params.handle)};
    if (!handle || !handle->allocated) {
        return NvResult::BadValue;
    }

    const s64 buffer_offset = params.buffer_offset;
    const u64 size =
        params.mapping_size ? static_cast<u64>(params.mapping_size) : handle->orig_size;
    if (buffer_offset < 0 ||
        !Common::IsAligned(static_cast<u64>(buffer_offset), VM::YUZU_PAGESIZE) || size == 0 ||
        static_cast<u64>(buffer_offset) > handle->aligned_size ||
        size > handle->aligned_size - static_cast<u64>(buffer_offset)) {
        LOG_WARNING(Service_NVDRV, "Cannot map a region outside the nvmap handle");
        return NvResult::BadValue;
    }

    const bool big_page = Common::IsAligned(handle->align, vm.big_page_size);
    if (!big_page && !Common::IsAligned(handle->align, VM::YUZU_PAGESIZE)) {
        return NvResult::BadValue;
    }
    bool use_big_pages = big_page;
    Allocation* allocation{};

    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None) {
        if (params.offset < 0 ||
            !Common::IsAligned(static_cast<u64>(params.offset), VM::YUZU_PAGESIZE)) {
            return NvResult::BadValue;
        }
        auto alloc{allocation_map.upper_bound(params.offset)};
        if (alloc == allocation_map.begin()) {
            LOG_WARNING(Service_NVDRV, "Cannot map into an unallocated GPU region");
            return NvResult::BadValue;
        }
        --alloc;
        allocation = &alloc->second;
        use_big_pages = allocation->big_pages && big_page;
        const u64 relative_offset = static_cast<u64>(params.offset) - alloc->first;
        if (relative_offset > allocation->size || size > allocation->size - relative_offset) {
            LOG_WARNING(Service_NVDRV, "Cannot map beyond the reserved GPU region");
            return NvResult::BadValue;
        }
    }

    // A fixed mapping consumes no new allocator pages. Preserve its exact
    // small-page coverage; MapRange keeps aligned interior big pages available.
    const u32 page_size =
        allocation ? VM::YUZU_PAGESIZE : (use_big_pages ? vm.big_page_size : VM::YUZU_PAGESIZE);
    const u32 page_size_bits = allocation
                                   ? VM::PAGE_SIZE_BITS
                                   : (use_big_pages ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS);
    if (size > std::numeric_limits<u64>::max() - (page_size - 1)) {
        return NvResult::BadValue;
    }
    const u64 mapped_size = Common::AlignUp(size, page_size);
    if (mapped_size > handle->aligned_size - static_cast<u64>(buffer_offset) ||
        (mapped_size >> page_size_bits) > std::numeric_limits<u32>::max()) {
        return NvResult::BadValue;
    }
    if (allocation) {
        const auto alloc = std::prev(allocation_map.upper_bound(params.offset));
        const u64 relative_offset = static_cast<u64>(params.offset) - alloc->first;
        if (mapped_size > allocation->size - relative_offset) {
            return NvResult::BadValue;
        }
        if (const auto existing = mapping_map.find(params.offset);
            existing != mapping_map.end() && !existing->second.fixed) {
            return NvResult::BadValue;
        }

        // Only the mapping at this exact start may be replaced. Partial overlaps
        // would leave two owners whose later unmaps clear each other's pages.
        const u64 start = static_cast<u64>(params.offset);
        const auto next = mapping_map.upper_bound(start);
        if (next != mapping_map.end() && next->first - start < mapped_size) {
            LOG_WARNING(Service_NVDRV, "Cannot overlap another GPU buffer at {:#X}", start);
            return NvResult::BadValue;
        }
        if (next != mapping_map.begin()) {
            const auto previous = std::prev(next);
            if (previous->first != start && previous->second.size > start - previous->first) {
                LOG_WARNING(Service_NVDRV, "Cannot overlap another GPU buffer at {:#X}", start);
                return NvResult::BadValue;
            }
        }
    }

    const DAddr base = nvmap.PinHandle(params.handle, false);
    if (base == 0) {
        return NvResult::InsufficientMemory;
    }
    auto pin_guard = SCOPE_GUARD {
        nvmap.UnpinHandle(params.handle);
    };
    const DAddr device_address = base + static_cast<u64>(buffer_offset);

    if (!allocation) {
        auto& allocator = use_big_pages ? *vm.big_page_allocator : *vm.small_page_allocator;
        const auto address = allocator.Allocate(static_cast<u32>(mapped_size >> page_size_bits));
        params.offset = static_cast<u64>(address) << page_size_bits;
        if (!address) {
            LOG_ERROR(Service_NVDRV, "Failed to allocate free space in the GPU AS");
            return NvResult::InsufficientMemory;
        }
    } else {
        // Pin the replacement before releasing the previous mapping. A failed pin
        // must leave the existing mapping and its owner intact.
        static_cast<void>(FreeMappingLocked(params.offset));
    }
    gmmu->Map(params.offset, device_address, mapped_size, static_cast<Tegra::PTEKind>(params.kind),
              use_big_pages);
    ReleaseRemapPinsLocked(params.offset, mapped_size);
    if (allocation) {
        allocation->mappings.push_back(params.offset);
    }
    mapping_map.insert_or_assign(params.offset,
                                 Mapping(params.handle, device_address, params.offset, mapped_size,
                                         allocation != nullptr, use_big_pages,
                                         allocation && allocation->sparse));
    map_buffer_offsets.insert(params.offset);
    pin_guard.Cancel();
    return NvResult::Success;
}

NvResult nvhost_as_gpu::UnmapBuffer(IoctlUnmapBuffer& params) {
    std::scoped_lock lock(mutex);
    if (map_buffer_offsets.contains(params.offset)) {
        LOG_DEBUG(Service_NVDRV, "called, offset={:#X}", params.offset);
        if (!vm.initialised) {
            return NvResult::BadValue;
        }

        static_cast<void>(FreeMappingLocked(params.offset));
    }
    return NvResult::Success;
}

NvResult nvhost_as_gpu::BindChannel(IoctlBindChannel& params) {
    LOG_DEBUG(Service_NVDRV, "called, fd={:X}", params.fd);

    std::scoped_lock lock(mutex);
    if (!vm.initialised) {
        return NvResult::BadValue;
    }
    const auto device = module.GetDevice<nvdevice>(params.fd);
    const auto result = device ? device->BindGpuAddressSpace(gmmu) : NvResult::BadValue;
    if (result != NvResult::Success) {
        LOG_WARNING(Service_NVDRV, "Cannot bind GPU address space to channel fd={}", params.fd);
    }
    return result;
}

void nvhost_as_gpu::GetVARegionsImpl(IoctlGetVaRegions& params) {
    params.buf_size = 2 * sizeof(VaRegion);

    params.regions = std::array<VaRegion, 2>{
        VaRegion{
            .offset = u64{vm.small_page_allocator->GetVAStart()} << VM::PAGE_SIZE_BITS,
            .page_size = VM::YUZU_PAGESIZE,
            ._pad0_{},
            .pages = vm.small_page_allocator->GetVALimit() - vm.small_page_allocator->GetVAStart(),
        },
        VaRegion{
            .offset = u64{vm.big_page_allocator->GetVAStart()} << vm.big_page_size_bits,
            .page_size = vm.big_page_size,
            ._pad0_{},
            .pages = vm.big_page_allocator->GetVALimit() - vm.big_page_allocator->GetVAStart(),
        },
    };
}

NvResult nvhost_as_gpu::GetVARegions1(IoctlGetVaRegions& params) {
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr, params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    return NvResult::Success;
}

NvResult nvhost_as_gpu::GetVARegions3(IoctlGetVaRegions& params, std::span<VaRegion> regions) {
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr,
              params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    const size_t num_regions = (std::min)(params.regions.size(), regions.size());
    for (size_t i = 0; i < num_regions; i++) {
        regions[i] = params.regions[i];
    }

    return NvResult::Success;
}

Kernel::KEvent* nvhost_as_gpu::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown AS GPU Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
