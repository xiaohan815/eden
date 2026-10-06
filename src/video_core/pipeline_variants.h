// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <vector>

namespace VideoCommon {

template <typename Pipeline, typename Failure>
struct PipelineVariants {
    // Retain successful variants while queued GPU work still references them.
    std::vector<std::unique_ptr<Pipeline>> pipelines;
    std::vector<Failure> failures;

    // No value means an untested specialization. A null pointer is a cached failure.
    template <typename PipelineMatches, typename FailureMatches>
    std::optional<Pipeline*> Find(const PipelineMatches& pipeline_matches,
                                 const FailureMatches& failure_matches) const {
        bool permanent_failure{};
        for (const auto& pipeline : pipelines) {
            if (!pipeline) {
                permanent_failure = true;
            } else if (pipeline_matches(*pipeline)) {
                return pipeline.get();
            }
        }
        if (permanent_failure) {
            return nullptr;
        }
        for (const auto& failure : failures) {
            if (failure_matches(failure)) {
                return nullptr;
            }
        }
        return std::nullopt;
    }
};

} // namespace VideoCommon
