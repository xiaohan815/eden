// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <barrier>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "video_core/host1x/host1x.h"

namespace {

struct NvdrvContext {
    NvdrvContext() : system{std::make_unique<Core::System>()} {
        system->Initialize();
        system->Kernel().Initialize();
        host1x = std::make_unique<Tegra::Host1x::Host1x>(*system);
        module = std::make_unique<Service::Nvidia::Module>(*system, *host1x);
    }

    ~NvdrvContext() {
        module.reset();
        host1x.reset();
        system->Kernel().Shutdown();
    }

    std::unique_ptr<Core::System> system;
    std::unique_ptr<Tegra::Host1x::Host1x> host1x;
    std::unique_ptr<Service::Nvidia::Module> module;
};

} // namespace

TEST_CASE("NVDRV keeps device ownership after descriptor removal", "[nvdrv]") {
    using namespace Service::Nvidia;
    NvdrvContext context;
    auto& module = *context.module;
    const auto fd = module.Open("/dev/nvdisp_disp0", {});
    REQUIRE(fd > 0);
    auto device = module.GetDevice<Devices::nvdisp_disp0>(fd);
    REQUIRE(device != nullptr);
    const std::weak_ptr<Devices::nvdisp_disp0> weak_device = device;

    REQUIRE(module.Close(fd) == NvResult::Success);
    REQUIRE(module.GetDevice<Devices::nvdisp_disp0>(fd) == nullptr);
    REQUIRE(module.VerifyFD(fd) == NvResult::NotImplemented);
    REQUIRE_FALSE(weak_device.expired());
    device.reset();
    REQUIRE(weak_device.expired());
    REQUIRE(module.Close(fd) == NvResult::NotImplemented);
}

TEST_CASE("NVDRV descriptor queries survive concurrent table growth and removal", "[nvdrv]") {
    using namespace Service::Nvidia;
    NvdrvContext context;
    auto& module = *context.module;
    const auto stable_fd = module.Open("/dev/nvdisp_disp0", {});
    const auto expected = module.GetDevice<Devices::nvdisp_disp0>(stable_fd);
    REQUIRE(expected != nullptr);

    constexpr std::size_t descriptor_count = 32768;
    std::barrier start{4};
    std::atomic<bool> finished{};
    std::atomic<unsigned> errors{};
    std::atomic<std::size_t> lookups{};
    const auto reader = [&] {
        start.arrive_and_wait();
        do {
            const auto device = module.GetDevice<Devices::nvdisp_disp0>(stable_fd);
            if (device != expected || module.VerifyFD(stable_fd) != NvResult::Success) {
                ++errors;
            }
            Kernel::KEvent* event = nullptr;
            if (module.QueryEvent(stable_fd, 0, event) != NvResult::BadParameter) {
                ++errors;
            }
            ++lookups;
        } while (!finished.load());
    };
    std::jthread first_reader{reader};
    std::jthread second_reader{reader};
    std::jthread writer{[&] {
        start.arrive_and_wait();
        std::vector<DeviceFD> descriptors;
        descriptors.reserve(descriptor_count);
        for (std::size_t i = 0; i < descriptor_count; ++i) {
            const auto fd = module.Open("/dev/nvdisp_disp0", {});
            if (fd <= stable_fd) {
                ++errors;
            }
            descriptors.push_back(fd);
        }
        // Remove alternating entries first, exercising the dense table's relocation.
        for (std::size_t parity = 0; parity < 2; ++parity) {
            for (std::size_t i = parity; i < descriptors.size(); i += 2) {
                if (module.Close(descriptors[i]) != NvResult::Success) {
                    ++errors;
                }
            }
        }
        finished = true;
    }};
    start.arrive_and_wait();
    writer.join();
    first_reader.join();
    second_reader.join();

    REQUIRE(errors.load() == 0);
    REQUIRE(lookups.load() > 0);
    REQUIRE(module.GetDevice<Devices::nvdisp_disp0>(stable_fd) == expected);
    REQUIRE(module.Close(stable_fd) == NvResult::Success);
}

TEST_CASE("NVDRV concurrent opens allocate distinct descriptors", "[nvdrv]") {
    using namespace Service::Nvidia;
    NvdrvContext context;
    auto& module = *context.module;
    constexpr std::size_t count = 4096;
    std::vector<DeviceFD> first(count), second(count);
    std::barrier start{3};
    const auto open = [&](std::vector<DeviceFD>& descriptors) {
        start.arrive_and_wait();
        for (auto& fd : descriptors) {
            fd = module.Open("/dev/nvdisp_disp0", {});
        }
    };
    std::jthread first_writer{[&] { open(first); }};
    std::jthread second_writer{[&] { open(second); }};
    start.arrive_and_wait();
    first_writer.join();
    second_writer.join();
    first.insert(first.end(), second.begin(), second.end());
    std::sort(first.begin(), first.end());
    REQUIRE(first.front() > 0);
    REQUIRE(std::adjacent_find(first.begin(), first.end()) == first.end());
    std::size_t closed{};
    for (const auto fd : first) {
        closed += module.Close(fd) == NvResult::Success;
    }
    REQUIRE(closed == 2 * count);
}
