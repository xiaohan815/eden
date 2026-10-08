// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <limits>
#include <optional>

#include "common/common_types.h"

namespace Vulkan::DisplayPacing {

constexpr u64 Period = 33'333'333;
constexpr u64 Phase = 20'000'000;
constexpr u64 ClockTimeout = 100'000'000;

/// Only advance to the next display-clock slot; never replay missed slots or
/// wait on a stale/forward clock. One renderer thread owns last_deadline.
inline std::optional<u64> NextDeadline(u64 now, u64 timestamp, u64 updated,
                                      u64 last_deadline) {
    if (!timestamp || !updated || updated > now || timestamp > now ||
        now - updated > ClockTimeout || now - timestamp > ClockTimeout ||
        now > std::numeric_limits<u64>::max() - 2 * Period || last_deadline > now + Period) {
        return std::nullopt;
    }
    u64 target = timestamp + Phase;
    if (target < now) {
        target += ((now - target - 1) / Period + 1) * Period;
    }
    if (target <= last_deadline) {
        target += ((last_deadline - target) / Period + 1) * Period;
    }
    // A clock discontinuity or a previously queued future deadline must not
    // turn into an unbounded wait. Fall back to the ordinary CPU pacer.
    if (target < now || target - now > Period) {
        return std::nullopt;
    }
    return target;
}

} // namespace Vulkan::DisplayPacing
