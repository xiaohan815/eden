// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <future>
#include <semaphore>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/fence_completion.h"

using namespace std::chrono_literals;

TEST_CASE("Completed fence waits do not issue extra work", "[video_core][fence]") {
    VideoCommon::FenceCompletion completion;
    completion.Wait(0);
    CHECK(completion.Issued() == 0);
    CHECK(completion.Issue() == 1);
    completion.Complete();
    for (int i = 0; i < 100; ++i) {
        completion.Wait(completion.Issued());
    }
    CHECK(completion.Issued() == 1);
}

TEST_CASE("Fence completion publishes callbacks in issue order", "[video_core][fence]") {
    VideoCommon::FenceCompletion completion;
    const auto first = completion.Issue();
    const auto second = completion.Issue();
    std::array<int, 2> guest_memory{};
    std::binary_semaphore finish_first{0}, finish_second{0};
    std::promise<int> first_result, second_result;
    auto first_ready = first_result.get_future();
    auto second_ready = second_result.get_future();
    std::jthread consumer([&] {
        finish_first.acquire();
        guest_memory[0] = 41;
        completion.Complete();
        finish_second.acquire();
        guest_memory[1] = 42;
        completion.Complete();
    });
    std::jthread first_waiter([&] {
        completion.Wait(first);
        first_result.set_value(guest_memory[0]);
    });
    std::jthread second_waiter([&] {
        completion.Wait(second);
        second_result.set_value(guest_memory[1]);
    });

    finish_first.release();
    const auto first_status = first_ready.wait_for(2s);
    const auto second_status = second_ready.wait_for(20ms);
    // Always release the consumer before checking, so a failed check can still join threads.
    finish_second.release();
    consumer.join();
    first_waiter.join();
    second_waiter.join();
    CHECK(first_status == std::future_status::ready);
    CHECK(second_status == std::future_status::timeout);
    CHECK(first_ready.get() == 41);
    CHECK(second_ready.get() == 42);
}

TEST_CASE("One completed fence releases all reached waiters", "[video_core][fence]") {
    VideoCommon::FenceCompletion completion;
    const auto target = completion.Issue();
    std::atomic<unsigned> ready{}, finished{};
    int guest_value{};
    std::vector<std::jthread> waiters;
    for (unsigned i = 0; i < 8; ++i) {
        waiters.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            completion.Wait(target);
            if (guest_value == 42) {
                finished.fetch_add(1, std::memory_order_release);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != 8) {
        std::this_thread::yield();
    }
    guest_value = 42;
    completion.Complete();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (finished.load(std::memory_order_acquire) != 8 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto resumed = finished.load(std::memory_order_acquire);
    // Supply a cleanup notification even when testing a broken wake-one implementation.
    completion.Issue();
    completion.Complete();
    waiters.clear();
    CHECK(resumed == 8);
}
