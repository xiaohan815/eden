// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

namespace Vulkan {

/// Opt-in 30 FPS pacing for the tested 120 Hz Metal surface. All AppKit operations
/// are asynchronous on the main thread, so renderer teardown never waits for Qt.
class MacOSDisplayPacer {
public:
    explicit MacOSDisplayPacer(void* metal_layer);
    ~MacOSDisplayPacer();

    /// Tests whether a live, compatible display clock can supply the frame delay.
    bool IsReady(bool eligible);

    /// Waits immediately before submitting the swapchain copy, after GPU resource
    /// waits and acquisition of the Vulkan submission lock.
    /// Returns true when display pacing replaces the ordinary CPU frame delay.
    /// GPU resource completion waits must still run.
    bool Wait(bool eligible);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
