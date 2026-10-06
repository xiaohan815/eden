// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"
#include "common/literals.h"

namespace Vulkan {

/// Selects a cache budget from the driver's remaining unified-memory budget.
/// This is a cache eviction threshold; it does not allocate or reserve memory.
[[nodiscard]] constexpr u64 CalculateAppleAggressiveMemoryBudget(u64 reported_budget,
                                                                u64 current_usage) {
    using namespace Common::Literals;
    constexpr u64 max_cache_budget = 16_GiB;
    constexpr u64 reserved_memory = 8_GiB;
    const u64 available_memory =
        reported_budget > current_usage ? reported_budget - current_usage : 0;
    const u64 usable_memory =
        available_memory > reserved_memory ? available_memory - reserved_memory : 0;
    return std::min(usable_memory, max_cache_budget);
}

} // namespace Vulkan
