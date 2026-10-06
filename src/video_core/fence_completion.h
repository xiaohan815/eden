// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCommon {

// One producer issues fences and one consumer completes them in issue order.
// Completion includes the fence's memory flushes and guest callbacks, not just GPU execution.
class FenceCompletion {
public:
    u64 Issue() noexcept {
        return ++issued;
    }

    u64 Issued() const noexcept {
        return issued;
    }

    void Complete() noexcept {
        completed.fetch_add(1, std::memory_order_release);
        completed.notify_all();
    }

    void Wait(u64 target) const noexcept {
        auto value = completed.load(std::memory_order_acquire);
        while (value < target) {
            completed.wait(value, std::memory_order_acquire);
            value = completed.load(std::memory_order_acquire);
        }
    }

private:
    u64 issued{}; // Only accessed by the producer.
    mutable std::atomic<u64> completed{};
};

} // namespace VideoCommon
