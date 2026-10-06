// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "video_core/vulkan_common/memory_budget.h"

using namespace Common::Literals;

TEST_CASE("Apple GPU cache budgets retain driver-reported headroom",
          "[video_core][memory_budget]") {
    using Vulkan::CalculateAppleAggressiveMemoryBudget;

    CHECK(CalculateAppleAggressiveMemoryBudget(96_GiB, 2_GiB) == 16_GiB);
    CHECK(CalculateAppleAggressiveMemoryBudget(24_GiB, 4_GiB) == 12_GiB);
    CHECK(CalculateAppleAggressiveMemoryBudget(16_GiB, 4_GiB) == 4_GiB);
    CHECK(CalculateAppleAggressiveMemoryBudget(12_GiB, 4_GiB) == 0);
    CHECK(CalculateAppleAggressiveMemoryBudget(12_GiB, 5_GiB) == 0);
    CHECK(CalculateAppleAggressiveMemoryBudget(8_GiB + 1, 0) == 1);
}

TEST_CASE("Apple GPU cache budgets do not wrap on stale memory reports",
          "[video_core][memory_budget]") {
    using Vulkan::CalculateAppleAggressiveMemoryBudget;

    CHECK(CalculateAppleAggressiveMemoryBudget(0, 0) == 0);
    CHECK(CalculateAppleAggressiveMemoryBudget(4_GiB, 8_GiB) == 0);
    CHECK(CalculateAppleAggressiveMemoryBudget(std::numeric_limits<u64>::max(), 0) == 16_GiB);
}
