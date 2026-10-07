// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/display_pacing.h"

using namespace Vulkan::DisplayPacing;

TEST_CASE("Display pacing rejects missing, stale and forward clocks", "[display_pacing]") {
    constexpr u64 now = 1'000'000'000;
    CHECK_FALSE(NextDeadline(now, 0, now, 0));
    CHECK_FALSE(NextDeadline(now, now, 0, 0));
    CHECK_FALSE(NextDeadline(now, now + 1, now, 0));
    CHECK_FALSE(NextDeadline(now, now, now + 1, 0));
    CHECK_FALSE(NextDeadline(now, now - ClockTimeout - 1, now, 0));
    CHECK_FALSE(NextDeadline(now, now, now - ClockTimeout - 1, 0));
}

TEST_CASE("Display pacing skips missed slots without accumulating delay", "[display_pacing]") {
    constexpr u64 stamp = 1'000'000'000;
    CHECK(NextDeadline(stamp, stamp, stamp, 0) == stamp + Phase);
    CHECK(NextDeadline(stamp + Phase, stamp, stamp, 0) == stamp + Phase);
    CHECK(NextDeadline(stamp + Phase + 1, stamp, stamp, 0) == stamp + Phase + Period);
    CHECK(NextDeadline(stamp + Phase + Period + 1, stamp, stamp, 0) ==
          stamp + Phase + 2 * Period);
    CHECK(NextDeadline(stamp + ClockTimeout, stamp, stamp, 0) == stamp + Phase + 3 * Period);
}

TEST_CASE("Display pacing never reuses a slot or waits beyond one period", "[display_pacing]") {
    constexpr u64 stamp = 1'000'000'000;
    const auto first = NextDeadline(stamp, stamp, stamp, 0);
    REQUIRE(first);
    CHECK(NextDeadline(stamp + Phase + 1, stamp, stamp, *first) == stamp + Phase + Period);
    CHECK(NextDeadline(stamp + Phase, stamp, stamp, *first) == stamp + Phase + Period);
    CHECK_FALSE(NextDeadline(stamp, stamp, stamp, *first));
    CHECK_FALSE(NextDeadline(stamp, stamp, stamp, stamp + 10 * Period));
}

TEST_CASE("Display pacing rejects overflowing clocks and deadlines", "[display_pacing]") {
    constexpr u64 maximum = std::numeric_limits<u64>::max();
    CHECK_FALSE(NextDeadline(maximum, maximum, maximum, 0));
    CHECK_FALSE(NextDeadline(maximum - Phase, maximum - Phase, maximum - Phase, 0));
    CHECK_FALSE(NextDeadline(1'000'000'000, 1'000'000'000, 1'000'000'000, maximum));
}
