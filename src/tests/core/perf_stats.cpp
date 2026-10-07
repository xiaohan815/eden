// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/perf_stats.h"

namespace {

struct Recording {
    Recording()
        : original_path{Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir)},
          original_setting{Settings::values.record_frame_times} {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-perf-stats-test-" + std::to_string(suffix));
        std::filesystem::create_directory(path);
        Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, path);
        Settings::values.record_frame_times = true;
    }

    ~Recording() {
        Settings::values.record_frame_times = original_setting;
        Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, original_path);
        std::filesystem::remove_all(path);
    }

    std::vector<double> ReadSamples() const {
        std::vector<double> samples;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            REQUIRE(entry.path().extension() == ".csv");
            std::ifstream file{entry.path()};
            double value{};
            while (file >> value) {
                samples.push_back(value);
            }
            CHECK(file.eof());
        }
        return samples;
    }

    std::filesystem::path original_path;
    std::filesystem::path path;
    bool original_setting;
};

constexpr u64 TEST_TITLE = 0xF001000000000001;

void RecordFrames(size_t frames) {
    // PerfStats owns a large history array; keep it off the test thread's stack.
    auto stats = std::make_unique<Core::PerfStats>(TEST_TITLE);
    for (size_t i = 0; i < frames; ++i) {
        stats->BeginSystemFrame();
        stats->EndSystemFrame();
    }
}

} // namespace

TEST_CASE("Frame-time recording handles shutdown during its warm-up frames",
          "[perf_stats][perf_stats_recording]") {
    const size_t frames = GENERATE(0, 1, 4, 5, 6, 7);
    Recording recording;
    RecordFrames(frames);
    const auto samples = recording.ReadSamples();
    CHECK(samples.size() == (frames > 5 ? frames - 5 : 0));
    if (frames <= 5) {
        CHECK(std::filesystem::is_empty(recording.path));
    }
    for (double value : samples) {
        CHECK(std::isfinite(value));
        CHECK(value >= 0);
    }
}

TEST_CASE("Frame-time recording stops at its history capacity",
          "[perf_stats][perf_stats_recording]") {
    Recording recording;
    RecordFrames(216000 + 7);
    const auto samples = recording.ReadSamples();
    REQUIRE(samples.size() == 216000 - 5);
    CHECK(std::isfinite(samples.front()));
    CHECK(std::isfinite(samples.back()));
}

TEST_CASE("Frame-time statistics remain finite when no frames arrive", "[perf_stats]") {
    auto stats = std::make_unique<Core::PerfStats>(0);

    const auto startup = stats->GetAndResetStats(std::chrono::microseconds{0});
    CHECK(std::isfinite(startup.frametime));
    CHECK(startup.frametime == 0.0);
    CHECK(startup.system_fps == 0.0);
    CHECK(startup.average_game_fps == 0.0);

    stats->BeginSystemFrame();
    stats->EndSystemFrame();
    stats->EndGameFrame();
    const auto active = stats->GetAndResetStats(std::chrono::microseconds{1});
    CHECK(std::isfinite(active.frametime));
    CHECK(active.frametime >= 0.0);
    CHECK(active.system_fps > 0.0);

    const auto idle = stats->GetAndResetStats(std::chrono::microseconds{2});
    CHECK(std::isfinite(idle.frametime));
    CHECK(idle.frametime == 0.0);
    CHECK(idle.system_fps == 0.0);
}
