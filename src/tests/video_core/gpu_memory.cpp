// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/host_memory.h"
#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace {

constexpr size_t PAGE = Core::DEVICE_PAGESIZE;
constexpr size_t BIG_PAGE = 64 * 1024;
constexpr VAddr CPU_BASE = 0x10000;
constexpr DAddr DEVICE_BASE = 0x20000;
constexpr GPUVAddr GPU_BASE = 0x30000;
constexpr PAddr PHYSICAL_BASE = 0x10000;
constexpr PAddr NEW_PHYSICAL = 0x60000;

class Rasterizer final : public VideoCore::RasterizerInterface {
public:
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& func) override {
        func();
    }
    void SyncOperation(std::function<void()>&& func) override {
        func();
    }
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {
        flushed.emplace_back(addr, size, which);
    }
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr, u64) override {
        return {};
    }
    void InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {
        invalidated.emplace_back(addr, size, which);
    }
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        return dma;
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

    std::vector<std::tuple<DAddr, u64, VideoCommon::CacheType>> flushed;
    std::vector<std::tuple<DAddr, u64, VideoCommon::CacheType>> invalidated;

private:
    Null::AccelerateDMA dma;
};

std::unique_ptr<Core::System> CreateSystem() {
    auto system = std::make_unique<Core::System>();
    system->Initialize();
    return system;
}

struct Memory {
    explicit Memory(bool big_pages = true)
        : system{CreateSystem()}, process{system->Kernel()}, device{system->DeviceMemory()},
          gpu{*system, device, 32} {
        device.BindInterface(&rasterizer);
        gpu.BindRasterizer(&rasterizer);
        auto& page_table = process.GetPageTable().GetImpl();
        page_table.Resize(20, Core::Memory::YUZU_PAGEBITS);
        auto& memory = process.GetMemory();
        memory.SetCurrentPageTable(process);
        memory.MapMemoryRegion(page_table, CPU_BASE, BIG_PAGE,
                               Core::DramMemoryMap::Base + PHYSICAL_BASE,
                               Common::MemoryPermission::ReadWrite, false);
        asid = device.RegisterProcess(&memory);
        device.Map(DEVICE_BASE, CPU_BASE, BIG_PAGE, asid, true);
        gpu.Map(GPU_BASE, DEVICE_BASE, BIG_PAGE, Tegra::PTEKind::INVALID, big_pages);
        for (size_t i = 0; i < BIG_PAGE / PAGE; ++i) {
            std::fill_n(system->DeviceMemory().GetPointerFromRaw<u8>(PHYSICAL_BASE + i * PAGE),
                        PAGE, static_cast<u8>((i + 1) * 0x11));
        }
        std::fill_n(system->DeviceMemory().GetPointerFromRaw<u8>(NEW_PHYSICAL), PAGE, 0x99);
        rasterizer.flushed.clear();
        rasterizer.invalidated.clear();
    }

    void UnmapPage(size_t index) {
        device.Unmap(DEVICE_BASE + index * PAGE, PAGE);
    }

    void RemapPage(size_t index) {
        UnmapPage(index);
        auto& memory = process.GetMemory();
        memory.MapMemoryRegion(process.GetPageTable().GetImpl(), CPU_BASE + index * PAGE, PAGE,
                               Core::DramMemoryMap::Base + NEW_PHYSICAL,
                               Common::MemoryPermission::ReadWrite, false);
        device.Map(DEVICE_BASE + index * PAGE, CPU_BASE + index * PAGE, PAGE, asid);
    }

    std::vector<u8> Read(GPUVAddr addr, size_t size, bool safe = false) {
        std::vector<u8> result(size, 0xFF);
        if (safe) {
            gpu.ReadBlock(addr, result.data(), size, VideoCommon::CacheType::BufferCache);
        } else {
            gpu.ReadBlockUnsafe(addr, result.data(), size);
        }
        return result;
    }

    std::unique_ptr<Core::System> system;
    Kernel::KProcess process;
    Rasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device;
    Tegra::MemoryManager gpu;
    Core::Asid asid{};
};

struct NvMemory : Memory {
    NvMemory() : host1x{*system}, core{host1x} {
        host1x.MemoryManager().BindInterface(&rasterizer);
        host1x.gmmu_manager.BindRasterizer(&rasterizer);
        session = core.OpenSession(&process);
    }

    ~NvMemory() {
        core.CloseSession(session);
    }

    auto CreateHandle(u64 size, VAddr address) {
        std::shared_ptr<Service::Nvidia::NvCore::NvMap::Handle> handle;
        REQUIRE(core.GetNvMapFile().CreateHandle(size, handle) ==
                Service::Nvidia::NvResult::Success);
        REQUIRE(handle->Alloc({}, PAGE, 0, address, session) == Service::Nvidia::NvResult::Success);
        return handle;
    }

    Tegra::Host1x::Host1x host1x;
    Service::Nvidia::NvCore::Container core;
    Service::Nvidia::NvCore::SessionId session{};
};

} // namespace

TEST_CASE("GPU block reads observe device holes after mapping", "[gpu_memory]") {
    const bool big_pages = GENERATE(true, false);
    const bool safe = GENERATE(true, false);
    Memory memory{big_pages};
    memory.UnmapPage(1);
    std::vector<u8> expected(PAGE * 3);
    std::fill_n(expected.begin(), PAGE, 0x11);
    std::fill_n(expected.begin() + PAGE * 2, PAGE, 0x33);
    REQUIRE(memory.Read(GPU_BASE, expected.size(), safe) == expected);
    REQUIRE(memory.Read(GPU_BASE + PAGE + 123, 257, safe) == std::vector<u8>(257));
}

TEST_CASE("GPU block reads observe changed physical backing", "[gpu_memory]") {
    const bool big_pages = GENERATE(true, false);
    const bool safe = GENERATE(true, false);
    Memory memory{big_pages};
    memory.RemapPage(1);
    std::vector<u8> expected(PAGE * 3);
    std::fill_n(expected.begin(), PAGE, 0x11);
    std::fill_n(expected.begin() + PAGE, PAGE, 0x99);
    std::fill_n(expected.begin() + PAGE * 2, PAGE, 0x33);
    REQUIRE(memory.Read(GPU_BASE, expected.size(), safe) == expected);
    const std::vector<u8> partial(expected.begin() + PAGE - 7, expected.begin() + PAGE + 7);
    REQUIRE(memory.Read(GPU_BASE + PAGE - 7, partial.size(), safe) == partial);
}

TEST_CASE("GPU block writes skip unmapped device backing", "[gpu_memory]") {
    const bool big_pages = GENERATE(true, false);
    Memory memory{big_pages};
    memory.UnmapPage(1);
    const std::vector<u8> data(PAGE * 3, 0x55);
    memory.gpu.WriteBlockUnsafe(GPU_BASE, data.data(), data.size());
    const u8* physical = memory.system->DeviceMemory().GetPointerFromRaw<u8>(PHYSICAL_BASE);
    REQUIRE(std::all_of(physical, physical + PAGE, [](u8 value) { return value == 0x55; }));
    REQUIRE(
        std::all_of(physical + PAGE, physical + 2 * PAGE, [](u8 value) { return value == 0x22; }));
    REQUIRE(std::all_of(physical + 2 * PAGE, physical + 3 * PAGE,
                        [](u8 value) { return value == 0x55; }));
}

TEST_CASE("GPU block writes follow remapped backing across device pages", "[gpu_memory]") {
    const bool big_pages = GENERATE(true, false);
    const bool safe = GENERATE(true, false);
    Memory memory{big_pages};
    memory.RemapPage(1);
    const std::vector<u8> data(PAGE + 14, 0x55);
    if (safe) {
        memory.gpu.WriteBlock(GPU_BASE + PAGE - 7, data.data(), data.size());
    } else {
        memory.gpu.WriteBlockUnsafe(GPU_BASE + PAGE - 7, data.data(), data.size());
    }
    const u8* physical = memory.system->DeviceMemory().GetPointerFromRaw<u8>(PHYSICAL_BASE);
    const u8* replacement = memory.system->DeviceMemory().GetPointerFromRaw<u8>(NEW_PHYSICAL);
    REQUIRE(std::all_of(physical, physical + PAGE - 7, [](u8 value) { return value == 0x11; }));
    REQUIRE(
        std::all_of(physical + PAGE - 7, physical + PAGE, [](u8 value) { return value == 0x55; }));
    REQUIRE(
        std::all_of(physical + PAGE, physical + PAGE * 2, [](u8 value) { return value == 0x22; }));
    REQUIRE(std::all_of(replacement, replacement + PAGE, [](u8 value) { return value == 0x55; }));
    REQUIRE(std::all_of(physical + PAGE * 2, physical + PAGE * 2 + 7,
                        [](u8 value) { return value == 0x55; }));
    REQUIRE(physical[PAGE * 2 + 7] == 0x33);
}

TEST_CASE("GPU pointer ranges observe current device mapping", "[gpu_memory]") {
    Memory memory;
    REQUIRE(memory.gpu.IsGranularRange(GPU_BASE, PAGE * 3));
    REQUIRE(memory.gpu.GetSpan(GPU_BASE, PAGE * 3) != nullptr);
    memory.UnmapPage(1);
    REQUIRE(!memory.gpu.IsGranularRange(GPU_BASE, PAGE * 3));
    REQUIRE(memory.gpu.GetSpan(GPU_BASE, PAGE * 3) == nullptr);
    REQUIRE(memory.gpu.GetSpan(GPU_BASE + PAGE * 2, PAGE) != nullptr);
    memory.RemapPage(1);
    REQUIRE(!memory.gpu.IsGranularRange(GPU_BASE, PAGE * 3));
    REQUIRE(memory.gpu.GetSpan(GPU_BASE, PAGE * 3) == nullptr);
}

TEST_CASE("GPU safe block access preserves cache synchronization", "[gpu_memory]") {
    const bool big_pages = GENERATE(true, false);
    Memory memory{big_pages};
    memory.Read(GPU_BASE, PAGE * 3, true);
    const size_t chunks = big_pages ? 1 : 3;
    REQUIRE(memory.rasterizer.flushed.size() == chunks);
    const auto read_bytes = big_pages ? PAGE * 3 : PAGE;
    REQUIRE(memory.rasterizer.flushed.front() ==
            std::tuple{DEVICE_BASE, read_bytes, VideoCommon::CacheType::BufferCache});
    const std::vector<u8> data(PAGE * 3, 0x55);
    memory.gpu.WriteBlock(GPU_BASE, data.data(), data.size(), VideoCommon::CacheType::QueryCache);
    REQUIRE(memory.rasterizer.invalidated.size() == chunks);
    REQUIRE(memory.rasterizer.invalidated.front() ==
            std::tuple{DEVICE_BASE, read_bytes, VideoCommon::CacheType::QueryCache});
}

TEST_CASE("GPU scalar reads follow both address translations across a page", "[gpu_memory]") {
    const bool big_pages = GENERATE(true, false);
    Memory memory{big_pages};
    memory.RemapPage(1);
    const auto check = [&]<typename T>() {
        std::array<u8, sizeof(T)> bytes;
        std::fill_n(bytes.begin(), sizeof(T) / 2, 0x11);
        std::fill(bytes.begin() + sizeof(T) / 2, bytes.end(), 0x99);
        T expected{};
        std::memcpy(&expected, bytes.data(), bytes.size());
        REQUIRE(memory.gpu.Read<T>(GPU_BASE + PAGE - sizeof(T) / 2) == expected);
    };
    check.operator()<u16>();
    check.operator()<u32>();
    check.operator()<u64>();
}

TEST_CASE("Device scalar reads follow remapped pages", "[gpu_memory]") {
    Memory memory;
    memory.RemapPage(1);
    const auto check = [&]<typename T>() {
        std::array<u8, sizeof(T)> bytes;
        std::fill_n(bytes.begin(), sizeof(T) / 2, 0x11);
        std::fill(bytes.begin() + sizeof(T) / 2, bytes.end(), 0x99);
        T expected{};
        std::memcpy(&expected, bytes.data(), bytes.size());
        REQUIRE(memory.device.Read<T>(DEVICE_BASE + PAGE - sizeof(T) / 2) == expected);
    };
    check.operator()<u16>();
    check.operator()<u32>();
    check.operator()<u64>();
}

TEST_CASE("Scalar writes preserve old backing when a device page moves", "[gpu_memory]") {
    const bool through_gpu = GENERATE(true, false);
    Memory memory;
    memory.RemapPage(1);
    constexpr u64 value = 0x0123456789ABCDEF;
    if (through_gpu) {
        memory.gpu.Write<u64>(GPU_BASE + PAGE - 4, value);
    } else {
        memory.device.Write<u64>(DEVICE_BASE + PAGE - 4, value);
    }
    const u8* physical = memory.system->DeviceMemory().GetPointerFromRaw<u8>(PHYSICAL_BASE);
    const u8* replacement = memory.system->DeviceMemory().GetPointerFromRaw<u8>(NEW_PHYSICAL);
    std::array<u8, sizeof(value)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(value));
    REQUIRE(std::equal(bytes.begin(), bytes.begin() + 4, physical + PAGE - 4));
    REQUIRE(
        std::all_of(physical + PAGE, physical + PAGE * 2, [](u8 byte) { return byte == 0x22; }));
    REQUIRE(std::equal(bytes.begin() + 4, bytes.end(), replacement));
}

TEST_CASE("GPU scalar reads honor noncontiguous small GPU pages", "[gpu_memory]") {
    Memory memory{false};
    memory.gpu.Map(GPU_BASE + PAGE, DEVICE_BASE + PAGE * 2, PAGE, Tegra::PTEKind::INVALID, false);
    REQUIRE(memory.gpu.Read<u64>(GPU_BASE + PAGE - 4) == 0x3333333311111111);
}

TEST_CASE("Scalar accesses crossing an unmapped device page preserve valid bytes", "[gpu_memory]") {
    const bool through_gpu = GENERATE(true, false);
    Memory memory;
    memory.UnmapPage(1);
    const u64 actual = through_gpu ? memory.gpu.Read<u64>(GPU_BASE + PAGE - 4)
                                   : memory.device.Read<u64>(DEVICE_BASE + PAGE - 4);
    REQUIRE(actual == 0x11111111);
    constexpr u64 value = 0x0123456789ABCDEF;
    if (through_gpu) {
        memory.gpu.Write<u64>(GPU_BASE + PAGE - 4, value);
    } else {
        memory.device.Write<u64>(DEVICE_BASE + PAGE - 4, value);
    }
    const u8* physical = memory.system->DeviceMemory().GetPointerFromRaw<u8>(PHYSICAL_BASE);
    REQUIRE(
        std::all_of(physical + PAGE, physical + PAGE * 2, [](u8 byte) { return byte == 0x22; }));
    const auto updated = through_gpu ? memory.gpu.Read<u64>(GPU_BASE + PAGE - 4)
                                     : memory.device.Read<u64>(DEVICE_BASE + PAGE - 4);
    REQUIRE(updated == 0x89ABCDEF);
}

TEST_CASE("GPU accesses stop at the address space boundary", "[gpu_memory]") {
    const bool big_pages = GENERATE(true, false);
    Memory memory{big_pages};
    constexpr GPUVAddr end = GPUVAddr{1} << 32;
    memory.gpu.Map(end - BIG_PAGE, DEVICE_BASE, BIG_PAGE, Tegra::PTEKind::INVALID, big_pages);
    std::vector<u8> expected(16, 0);
    std::fill_n(expected.begin(), 4, 0x10);
    REQUIRE(memory.Read(end - 4, expected.size()) == expected);
    REQUIRE(memory.gpu.Read<u64>(end - 4) == 0x10101010);
    REQUIRE(memory.Read(end, 16) == std::vector<u8>(16));
    REQUIRE(memory.gpu.GetSpan(end - 4, 16) == nullptr);
    REQUIRE(!memory.gpu.IsGranularRange(end - 4, 16));
    REQUIRE(memory.Read(~GPUVAddr{0} - 3, 16) == std::vector<u8>(16));
    constexpr u64 value = 0x0123456789ABCDEF;
    memory.gpu.Write<u64>(end - 4, value);
    REQUIRE(memory.gpu.Read<u64>(end - 4) == 0x89ABCDEF);
    memory.gpu.WriteBlockUnsafe(~GPUVAddr{0} - 3, &value, sizeof(value));
}

TEST_CASE("NvMap reclaims an idle handle when the device address space is full",
          "[gpu_memory][nvmap]") {
    Memory memory;
    Tegra::Host1x::Host1x host1x{*memory.system};
    auto& smmu = host1x.MemoryManager();
    smmu.BindInterface(&memory.rasterizer);
    Service::Nvidia::NvCore::Container core{host1x};
    auto& nvmap = core.GetNvMapFile();
    const auto session = core.OpenSession(&memory.process);
    using Handle = Service::Nvidia::NvCore::NvMap::Handle;
    std::shared_ptr<Handle> idle;
    std::shared_ptr<Handle> replacement;
    REQUIRE(nvmap.CreateHandle(PAGE, idle) == Service::Nvidia::NvResult::Success);
    REQUIRE(idle->Alloc({}, PAGE, 0, CPU_BASE, session) == Service::Nvidia::NvResult::Success);
    REQUIRE(nvmap.CreateHandle(PAGE, replacement) == Service::Nvidia::NvResult::Success);
    REQUIRE(replacement->Alloc({}, PAGE, 0, CPU_BASE + PAGE, session) ==
            Service::Nvidia::NvResult::Success);
    const DAddr address = nvmap.PinHandle(idle->id, false);
    REQUIRE(address != 0);
    const DAddr reserved_start = address + BIG_PAGE;
    const size_t reserved_size = (DAddr{1} << smmu.AS_BITS) - 1 - reserved_start;
    // Reserve virtual space only; no host memory or GPU allocation is needed.
    REQUIRE(smmu.Allocate(reserved_size) == reserved_start);
    REQUIRE(smmu.Allocate(BIG_PAGE) == 0);
    nvmap.UnpinHandle(idle->id);
    REQUIRE(idle->pins == 0);
    REQUIRE(idle->unmap_queue_entry.has_value());

    REQUIRE(nvmap.PinHandle(replacement->id, false) == address);
    REQUIRE(idle->d_address == 0);
    REQUIRE_FALSE(idle->unmap_queue_entry.has_value());
    REQUIRE(replacement->pins == 1);
    REQUIRE(smmu.Read<u8>(address) == 0x22);

    nvmap.UnpinHandle(replacement->id);
    core.CloseSession(session);
    smmu.Free(reserved_start, reserved_size);
}

TEST_CASE("NvMap reports a full address space without reclaimable handles", "[gpu_memory][nvmap]") {
    Memory memory;
    Tegra::Host1x::Host1x host1x{*memory.system};
    auto& smmu = host1x.MemoryManager();
    smmu.BindInterface(&memory.rasterizer);
    Service::Nvidia::NvCore::Container core{host1x};
    auto& nvmap = core.GetNvMapFile();
    const auto session = core.OpenSession(&memory.process);
    using Handle = Service::Nvidia::NvCore::NvMap::Handle;
    std::shared_ptr<Handle> active;
    std::shared_ptr<Handle> replacement;
    REQUIRE(nvmap.CreateHandle(PAGE, active) == Service::Nvidia::NvResult::Success);
    REQUIRE(active->Alloc({}, PAGE, 0, CPU_BASE, session) == Service::Nvidia::NvResult::Success);
    REQUIRE(nvmap.CreateHandle(PAGE, replacement) == Service::Nvidia::NvResult::Success);
    REQUIRE(replacement->Alloc({}, PAGE, 0, CPU_BASE + PAGE, session) ==
            Service::Nvidia::NvResult::Success);
    const DAddr address = nvmap.PinHandle(active->id, false);
    REQUIRE(address != 0);
    const DAddr reserved_start = address + BIG_PAGE;
    const size_t reserved_size = (DAddr{1} << smmu.AS_BITS) - 1 - reserved_start;
    REQUIRE(smmu.Allocate(reserved_size) == reserved_start);
    REQUIRE(nvmap.PinHandle(replacement->id, false) == 0);
    REQUIRE(active->pins == 1);
    REQUIRE(active->d_address == address);
    REQUIRE(smmu.Read<u8>(address) == 0x11);
    REQUIRE(replacement->pins == 0);
    REQUIRE(replacement->d_address == 0);
    REQUIRE_FALSE(replacement->unmap_queue_entry.has_value());

    nvmap.UnpinHandle(active->id);
    core.CloseSession(session);
    smmu.Free(reserved_start, reserved_size);
}

TEST_CASE("NvMap low-area failure leaves no mapping and can be retried",
          "[gpu_memory][nvmap][nvmap_low]") {
    const bool cached = GENERATE(false, true);
    NvMemory memory;
    auto& nvmap = memory.core.GetNvMapFile();
    auto handle = memory.CreateHandle(PAGE, CPU_BASE);
    if (cached) {
        REQUIRE(nvmap.PinHandle(handle->id, false) != 0);
        nvmap.UnpinHandle(handle->id);
        REQUIRE(handle->unmap_queue_entry.has_value());
    }
    auto& allocator = memory.host1x.Allocator();
    const u32 reserved_start = allocator.GetVAStart();
    const u32 reserved_size = allocator.GetVALimit() - reserved_start;
    REQUIRE(allocator.Allocate(reserved_size) == reserved_start);
    REQUIRE(nvmap.PinHandle(handle->id, true) == 0);
    CHECK(handle->pins == 0);
    CHECK(handle->d_address == 0);
    CHECK(handle->pin_virt_address == 0);
    CHECK_FALSE(handle->unmap_queue_entry.has_value());
    CHECK_FALSE(memory.host1x.gmmu_manager.GpuToCpuAddress(0).has_value());

    allocator.Free(reserved_start, reserved_size);
    const DAddr address = nvmap.PinHandle(handle->id, true);
    REQUIRE(address != 0);
    CHECK(handle->pins == 1);
    CHECK_FALSE(memory.host1x.gmmu_manager.GpuToCpuAddress(0).has_value());
    CHECK(memory.host1x.gmmu_manager.Read<u8>(address) == 0x11);
    nvmap.UnpinHandle(handle->id);
}

TEST_CASE("NvMap low-area failure preserves an existing device pin",
          "[gpu_memory][nvmap][nvmap_low]") {
    NvMemory memory;
    auto& nvmap = memory.core.GetNvMapFile();
    auto handle = memory.CreateHandle(PAGE, CPU_BASE);
    const DAddr device_address = nvmap.PinHandle(handle->id, false);
    REQUIRE(device_address != 0);
    auto& allocator = memory.host1x.Allocator();
    const u32 reserved_start = allocator.GetVAStart();
    const u32 reserved_size = allocator.GetVALimit() - reserved_start;
    REQUIRE(allocator.Allocate(reserved_size) == reserved_start);
    REQUIRE(nvmap.PinHandle(handle->id, true) == 0);
    CHECK(handle->pins == 1);
    CHECK(handle->d_address == device_address);
    CHECK(handle->pin_virt_address == 0);
    CHECK(memory.host1x.MemoryManager().Read<u8>(device_address) == 0x11);
    CHECK_FALSE(memory.host1x.gmmu_manager.GpuToCpuAddress(0).has_value());
    allocator.Free(reserved_start, reserved_size);
    const DAddr address = nvmap.PinHandle(handle->id, true);
    REQUIRE(address != 0);
    CHECK(handle->pins == 2);
    CHECK(memory.host1x.gmmu_manager.Read<u8>(address) == 0x11);
    nvmap.UnpinHandle(handle->id);
    nvmap.UnpinHandle(handle->id);
}

TEST_CASE("NvMap reclaims idle low-area mappings before reporting exhaustion",
          "[gpu_memory][nvmap][nvmap_low]") {
    NvMemory memory;
    auto& nvmap = memory.core.GetNvMapFile();
    auto device_only = memory.CreateHandle(PAGE, CPU_BASE + PAGE * 2);
    const DAddr device_only_address = nvmap.PinHandle(device_only->id, false);
    REQUIRE(device_only_address != 0);
    nvmap.UnpinHandle(device_only->id);
    auto idle = memory.CreateHandle(PAGE, CPU_BASE);
    auto replacement = memory.CreateHandle(PAGE, CPU_BASE + PAGE);
    const DAddr address = nvmap.PinHandle(idle->id, true);
    REQUIRE(address != 0);
    nvmap.UnpinHandle(idle->id);
    auto& allocator = memory.host1x.Allocator();
    const u32 reserved_start = static_cast<u32>(address) + PAGE;
    const u32 reserved_size = allocator.GetVALimit() - reserved_start;
    REQUIRE(allocator.Allocate(reserved_size) == reserved_start);

    REQUIRE(nvmap.PinHandle(replacement->id, true) == address);
    CHECK(idle->d_address == 0);
    CHECK(idle->pin_virt_address == 0);
    CHECK_FALSE(idle->unmap_queue_entry.has_value());
    CHECK(replacement->pins == 1);
    CHECK(memory.host1x.gmmu_manager.Read<u8>(address) == 0x22);
    CHECK(device_only->d_address == device_only_address);
    CHECK(device_only->pins == 0);
    CHECK(device_only->unmap_queue_entry.has_value());
    CHECK(memory.host1x.MemoryManager().Read<u8>(device_only_address) == 0x33);
    nvmap.UnpinHandle(replacement->id);
    allocator.Free(reserved_start, reserved_size);
}

TEST_CASE("NvMap low-area pin rejects sizes beyond its address width",
          "[gpu_memory][nvmap][nvmap_low]") {
    NvMemory memory;
    auto& nvmap = memory.core.GetNvMapFile();
    auto handle = memory.CreateHandle((u64{1} << 32) + PAGE, CPU_BASE);
    REQUIRE(nvmap.PinHandle(handle->id, true) == 0);
    CHECK(handle->pins == 0);
    CHECK(handle->d_address == 0);
    CHECK(handle->pin_virt_address == 0);
    CHECK_FALSE(handle->unmap_queue_entry.has_value());
    CHECK_FALSE(memory.host1x.gmmu_manager.GpuToCpuAddress(0).has_value());
}
