// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Vulkan {

// Tracks confirmed GPU completion. A single completed submission can satisfy
// multiple resource and fence waiters, even if no further work is submitted.
class GpuCompletion {
public:
    [[nodiscard]] u64 Value() const noexcept {
        return value.load(std::memory_order_acquire);
    }

    void Refresh(u64 tick) noexcept {
        auto observed = value.load(std::memory_order_relaxed);
        while (observed < tick) {
            if (value.compare_exchange_weak(observed, tick, std::memory_order_release,
                                           std::memory_order_relaxed)) {
                value.notify_all();
                return;
            }
        }
    }

    void Wait(u64 tick) const noexcept {
        auto observed = Value();
        while (observed < tick) {
            value.wait(observed, std::memory_order_acquire);
            observed = Value();
        }
    }

private:
    mutable std::atomic<u64> value{};
};

} // namespace Vulkan
