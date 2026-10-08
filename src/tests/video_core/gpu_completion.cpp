// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/gpu_completion.h"

using namespace std::chrono_literals;

TEST_CASE("GPU completion never moves backwards", "[video_core][gpu_completion]") {
    Vulkan::GpuCompletion completion;
    CHECK(completion.Value() == 0);
    completion.Wait(0);
    completion.Refresh(7);
    completion.Refresh(3);
    completion.Refresh(7);
    completion.Wait(7);
    CHECK(completion.Value() == 7);
}

TEST_CASE("One GPU submission releases all reached resource waiters",
          "[video_core][gpu_completion]") {
    Vulkan::GpuCompletion completion;
    std::latch entered{8};
    std::atomic<unsigned> finished{};
    int published_value{};
    std::vector<std::jthread> waiters;
    for (unsigned i = 0; i < 8; ++i) {
        waiters.emplace_back([&] {
            entered.count_down();
            completion.Wait(1);
            if (published_value == 42) {
                finished.fetch_add(1, std::memory_order_release);
            }
        });
    }
    entered.wait();
    // Let the waiters park before completing the only submission in this case.
    std::this_thread::sleep_for(20ms);
    published_value = 42;
    completion.Refresh(1);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (finished.load(std::memory_order_acquire) != 8 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto resumed = finished.load(std::memory_order_acquire);
    // Also allow a failing wake-one variant to join without hanging the test.
    for (u64 tick = 2; tick <= 10; ++tick) {
        completion.Refresh(tick);
        std::this_thread::sleep_for(1ms);
    }
    waiters.clear();
    CHECK(resumed == 8);
}

TEST_CASE("GPU completion does not release future submissions",
          "[video_core][gpu_completion]") {
    Vulkan::GpuCompletion completion;
    std::promise<void> reached, future;
    auto reached_result = reached.get_future();
    auto future_result = future.get_future();
    std::jthread first([&] {
        completion.Wait(3);
        reached.set_value();
    });
    std::jthread second([&] {
        completion.Wait(5);
        future.set_value();
    });
    completion.Refresh(3);
    const auto reached_status = reached_result.wait_for(2s);
    const auto future_status = future_result.wait_for(20ms);
    completion.Refresh(5);
    // Repeated notifications also make test cleanup tolerate wake-one mutants.
    for (u64 tick = 6; tick <= 8; ++tick) {
        completion.Refresh(tick);
    }
    first.join();
    second.join();
    CHECK(reached_status == std::future_status::ready);
    CHECK(future_status == std::future_status::timeout);
}
