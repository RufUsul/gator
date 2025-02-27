/* Copyright (C) 2016-2022 by Arm Limited. All rights reserved. */

#include "mali_userspace/MaliInstanceLocator.h"

#include "DynBuf.h"
#include "Logging.h"
#include "lib/FsEntry.h"
#include "lib/String.h"
#include "mali_userspace/MaliDeviceApi.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mali_userspace {
    static void enumerateMaliGpuClockPaths(const lib::FsEntry & currentDirectory,
                                           std::map<unsigned int, std::string> & gpuClockPaths)
    {
        // open sysfs directory
        if (currentDirectory.read_stats().type() != lib::FsEntry::Type::DIR) {
            LOG_DEBUG("Failed to open '%s'", currentDirectory.path().c_str());
            return;
        }

        // is the parent called 'misc'
        const bool dirIsCalledMisc = (currentDirectory.name() == "misc");
        const std::optional<lib::FsEntry> dirsParent = currentDirectory.parent();
        const std::optional<lib::FsEntry> parentClockPath =
            (dirsParent ? std::optional<lib::FsEntry> {lib::FsEntry::create(*dirsParent, "clock")}
                        : std::optional<lib::FsEntry> {});

        // walk children looking for directories named 'mali%d'
        lib::FsEntryDirectoryIterator iterator = currentDirectory.children();
        std::optional<lib::FsEntry> childEntry;
        while (!!(childEntry = iterator.next())) {
            // determine type
            lib::FsEntry::Stats childStats = childEntry->read_stats();
            if (childStats.type() == lib::FsEntry::Type::DIR) {
                // check name is 'mali#'
                unsigned int id = 0;
                // NOLINTNEXTLINE(cert-err34-c)
                if (dirIsCalledMisc && sscanf(childEntry->name().c_str(), "mali%u", &id) == 1) {
                    // don't repeat your self
                    if (gpuClockPaths.count(id) > 0) {
                        continue;
                    }
                    // check for 'misc/clock' directory
                    lib::FsEntry childClockPath = lib::FsEntry::create(*childEntry, "clock");
                    if (childClockPath.exists() && childClockPath.canAccess(true, false, false)) {
                        gpuClockPaths[id] = childClockPath.path();
                    }
                    // use ../../clock ?
                    else if (parentClockPath && parentClockPath->exists()
                             && parentClockPath->canAccess(true, false, false)) {
                        gpuClockPaths[id] = parentClockPath->path();
                    }
                }
                // try to recursively scan the child directory
                else if (!childStats.is_symlink()) {
                    enumerateMaliGpuClockPaths(*childEntry, gpuClockPaths);
                }
            }
        }
    }

    std::map<unsigned int, std::unique_ptr<MaliDevice>> enumerateAllMaliHwCntrDrivers()
    {
        static constexpr unsigned int MAX_DEV_MALI_TOO_SCAN_FOR = 16;

        std::map<unsigned int, std::unique_ptr<IMaliDeviceApi>> detectedDevices;
        std::map<unsigned int, std::string> gpuClockPaths;
        std::map<unsigned int, std::unique_ptr<MaliDevice>> coreDriverMap;

        // first scan for '/dev/mali#' files
        for (unsigned int i = 0; i < MAX_DEV_MALI_TOO_SCAN_FOR; ++i) {
            // construct the path
            lib::printf_str_t<16> pathBuffer {"/dev/mali%u", i};
            // attempt to open device
            std::unique_ptr<IMaliDeviceApi> device = IMaliDeviceApi::probe(pathBuffer);
            if (device) {
                detectedDevices[i] = std::move(device);
            }
        }

        if (!detectedDevices.empty()) {
            // now scan /sys to find the 'clock' metadata files from which we read gpu frequency
            enumerateMaliGpuClockPaths(lib::FsEntry::create("/sys"), gpuClockPaths);

            // populate result
            for (auto & detectedDevice : detectedDevices) {
                const unsigned int id = detectedDevice.first;

                std::unique_ptr<MaliDevice> device =
                    MaliDevice::create(std::move(detectedDevice.second), std::move(gpuClockPaths[id]));

                if (device) {
                    coreDriverMap[id] = std::move(device);
                }
            }
        }

        return coreDriverMap;
    }
}
