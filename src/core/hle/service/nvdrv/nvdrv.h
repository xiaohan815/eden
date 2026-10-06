// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <ankerl/unordered_dense.h>

#include "common/common_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::Nvidia {

namespace NvCore {
class Container;
class SyncpointManager;
} // namespace NvCore

namespace Devices {
class nvdevice;
class nvhost_ctrl;
} // namespace Devices

class Module;

class EventInterface {
public:
    explicit EventInterface(Module& module_);
    ~EventInterface();

    Kernel::KEvent* CreateEvent(std::string name);

    void FreeEvent(Kernel::KEvent* event);

private:
    Module& module;
    std::mutex guard;
    std::list<Devices::nvhost_ctrl*> on_signal;
};

class Module final {
public:
    explicit Module(Core::System& system_);
    Module(Core::System& system_, Tegra::Host1x::Host1x& host1x);
    ~Module();

    /// Returns shared ownership of a device identified by its file descriptor.
    template <typename T>
    std::shared_ptr<T> GetDevice(DeviceFD fd) {
        // Copy ownership while the table is protected. Device callbacks must run
        // after releasing this lock, since they can look up other descriptors.
        std::scoped_lock lock{open_files_mutex};
        auto itr = open_files.find(fd);
        if (itr == open_files.end())
            return nullptr;
        return std::static_pointer_cast<T>(itr->second);
    }

    NvResult VerifyFD(DeviceFD fd) const;

    /// Opens a device node and returns a file descriptor to it.
    DeviceFD Open(const std::string& device_name, NvCore::SessionId session_id);

    /// Sends an ioctl command to the specified file descriptor.
    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output);

    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<const u8> inline_input, std::span<u8> output);

    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output);

    /// Closes a device file descriptor and returns operation success.
    NvResult Close(DeviceFD fd);

    NvResult QueryEvent(DeviceFD fd, u32 event_id, Kernel::KEvent*& event);

    NvCore::Container& GetContainer() {
        return container;
    }

private:
    friend class EventInterface;

    /// Manages syncpoints on the host
    NvCore::Container container;

    /// Id to use for the next open file descriptor.
    DeviceFD next_fd = 1;

    using FilesContainerType = ankerl::unordered_dense::map<DeviceFD, std::shared_ptr<Devices::nvdevice>>;
    /// Mapping of file descriptors to the devices they reference.
    mutable std::mutex open_files_mutex;
    FilesContainerType open_files;

    KernelHelpers::ServiceContext service_context;

    EventInterface events_interface;

    ankerl::unordered_dense::map<std::string, std::function<std::shared_ptr<Devices::nvdevice>()>>
        builders;
};

void LoopProcess(Core::System& system);

} // namespace Service::Nvidia
