// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <mach/mach_time.h>
#include <pthread.h>

#include "common/logging.h"
#include "video_core/renderer_vulkan/display_pacing.h"
#include "video_core/renderer_vulkan/macos_display_pacer.h"

namespace {

u64 Now() {
    return static_cast<u64>(CACurrentMediaTime() * 1e9);
}

struct ClockSnapshot {
    u64 timestamp{};
    u64 updated{};
    bool compatible{};
};

struct DisplayClock {
    std::mutex mutex;
    ClockSnapshot snapshot;
    std::atomic_bool stopped{};
    bool logged_initial{};
    // These Objective-C members are only touched/released on the main thread.
    __weak NSView* view;
    __strong CADisplayLink* link;
};

bool ContainsLayer(CALayer* root, CALayer* target) {
    if (root == target) {
        return true;
    }
    for (CALayer* child in root.sublayers) {
        if (ContainsLayer(child, target)) {
            return true;
        }
    }
    return false;
}

NSView* FindView(NSView* view, CAMetalLayer* layer) {
    for (NSView* child in view.subviews) {
        if (NSView* found = FindView(child, layer)) {
            return found;
        }
    }
    return ContainsLayer(view.layer, layer) ? view : nil;
}

} // namespace

@interface EdenDisplayPacingSink : NSObject {
@public
    std::shared_ptr<DisplayClock> clock;
}
- (void)tick:(CADisplayLink*)sender;
@end

@implementation EdenDisplayPacingSink
- (void)tick:(CADisplayLink*)sender {
    if (clock->stopped.load(std::memory_order_acquire)) {
        return;
    }
    NSView* view = clock->view;
    NSWindow* window = view.window;
    // The fixed phase is measured only on a 120 Hz display at 30 FPS. Follow
    // the NSView across screens, and fail closed for any other refresh period.
    bool compatible = false;
    if (@available(macOS 14.0, *)) {
        compatible = window.visible && !window.miniaturized &&
                     (window.occlusionState & NSWindowOcclusionStateVisible) &&
                     window.screen.maximumFramesPerSecond == 120 &&
                     std::abs(sender.duration - 1.0 / 120.0) < 0.0001 &&
                     std::abs(sender.targetTimestamp - sender.timestamp - 1.0 / 30.0) < 0.001;
    }
    std::scoped_lock lock{clock->mutex};
    if (!clock->logged_initial || compatible != clock->snapshot.compatible) {
        LOG_INFO(Render_Vulkan,
                 "Display pacing clock compatible={}, visible={}, minimized={}, occlusion={}, "
                 "maximum_fps={}, duration={}, target_interval={}",
                 compatible, static_cast<bool>(window.visible),
                 static_cast<bool>(window.miniaturized), static_cast<u64>(window.occlusionState),
                 static_cast<s64>(window.screen.maximumFramesPerSecond), sender.duration,
                 sender.targetTimestamp - sender.timestamp);
        clock->logged_initial = true;
    }
    clock->snapshot = {static_cast<u64>(sender.timestamp * 1e9), Now(), compatible};
}
@end

namespace Vulkan {

struct MacOSDisplayPacer::Impl {
    std::shared_ptr<DisplayClock> clock = std::make_shared<DisplayClock>();
    u64 last_deadline{};
    qos_class_t previous_qos = QOS_CLASS_DEFAULT;
    int previous_relative{};
    bool qos_changed{};
    bool qos_failed{};
    bool logged_qos{};

    void RestoreQoS() {
        if (qos_changed) {
            pthread_set_qos_class_self_np(previous_qos, previous_relative);
            qos_changed = false;
        }
        last_deadline = 0;
    }
};

MacOSDisplayPacer::MacOSDisplayPacer(void* metal_layer) : impl{std::make_unique<Impl>()} {
    auto clock = impl->clock;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (clock->stopped.load(std::memory_order_acquire)) {
            return;
        }
        if (@available(macOS 14.0, *)) {
            NSView* view = nil;
            for (NSWindow* window in NSApp.windows) {
                view = FindView(window.contentView, layer);
                if (view) {
                    break;
                }
            }
            if (!view || ![layer isKindOfClass:CAMetalLayer.class] ||
                !layer.displaySyncEnabled || layer.presentsWithTransaction) {
                LOG_INFO(Render_Vulkan, "Experimental macOS display pacing unavailable for surface");
                return;
            }
            auto sink = [EdenDisplayPacingSink new];
            sink->clock = clock;
            clock->view = view;
            clock->link = [view displayLinkWithTarget:sink selector:@selector(tick:)];
            clock->link.preferredFrameRateRange = CAFrameRateRangeMake(30, 30, 30);
            [clock->link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
            LOG_INFO(Render_Vulkan, "Experimental macOS display pacing registered (30 FPS, {} ms phase)",
                     DisplayPacing::Phase / 1'000'000);
        } else {
            LOG_INFO(Render_Vulkan, "Experimental macOS display pacing requires macOS 14 or later");
        }
    });
}

MacOSDisplayPacer::~MacOSDisplayPacer() {
    auto clock = impl->clock;
    clock->stopped.store(true, std::memory_order_release);
    // Never dispatch_sync: Qt may be joining the emulation thread right now.
    // This block owns the clock until invalidation breaks link -> sink -> clock.
    dispatch_async(dispatch_get_main_queue(), ^{
        [clock->link invalidate];
        clock->link = nil;
        clock->view = nil;
    });
}

bool MacOSDisplayPacer::IsReady(bool eligible) {
    std::scoped_lock lock{impl->clock->mutex};
    const auto& snapshot = impl->clock->snapshot;
    if (!eligible || !snapshot.compatible || impl->qos_failed ||
        !DisplayPacing::NextDeadline(Now(), snapshot.timestamp, snapshot.updated,
                                    impl->last_deadline)) {
        impl->RestoreQoS();
        return false;
    }
    return true;
}

bool MacOSDisplayPacer::Wait(bool eligible) {
    ClockSnapshot snapshot;
    {
        std::scoped_lock lock{impl->clock->mutex};
        snapshot = impl->clock->snapshot;
    }
    const auto deadline = eligible && snapshot.compatible
                              ? DisplayPacing::NextDeadline(Now(), snapshot.timestamp,
                                                           snapshot.updated, impl->last_deadline)
                              : std::nullopt;
    if (!deadline || impl->qos_failed) {
        impl->RestoreQoS();
        return false;
    }
    if (!impl->qos_changed) {
        pthread_get_qos_class_np(pthread_self(), &impl->previous_qos, &impl->previous_relative);
        if (impl->previous_qos == QOS_CLASS_UNSPECIFIED) {
            impl->previous_qos = QOS_CLASS_DEFAULT;
            impl->previous_relative = 0;
        }
        const int result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        if (result != 0) {
            impl->qos_failed = true;
            LOG_WARNING(Render_Vulkan, "macOS display pacing QoS failed: {}", result);
            return false;
        }
        impl->qos_changed = true;
        if (!impl->logged_qos) {
            LOG_INFO(Render_Vulkan, "Display pacing active on presentation thread with interactive QoS");
            impl->logged_qos = true;
        }
    }
    constexpr u64 spin_window = 500'000;
    static const auto timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    for (u64 now = Now(); now < *deadline; now = Now()) {
        const u64 remaining = *deadline - now;
        if (remaining > spin_window) {
            const u64 ticks = (remaining - spin_window) * timebase.denom / timebase.numer;
            if (mach_wait_until(mach_absolute_time() + ticks) != KERN_SUCCESS) {
                impl->RestoreQoS();
                return false;
            }
        } else {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
    impl->last_deadline = *deadline;
    return true;
}

} // namespace Vulkan
