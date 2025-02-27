/* Copyright (C) 2013-2022 by Arm Limited. All rights reserved. */

#include "CpuUtils.h"

#include "CpuUtils_Topology.h"
#include "Logging.h"
#include "OlyUtility.h"
#include "lib/File.h"
#include "linux/PerCoreIdentificationThread.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <dirent.h>
#include <unistd.h>

namespace cpu_utils {
    unsigned int getMaxCoreNum()
    {
        // why don't we just use /sys/devices/system/cpu/kernel_max
        // or pick the highest in /sys/devices/system/cpu/possible?
        DIR * dir = opendir("/sys/devices/system/cpu");
        if (dir == nullptr) {
            LOG_ERROR("Unable to determine the number of cores on the target, opendir failed");
            handleException();
        }

        long maxCoreNum = -1;
        struct dirent * dirent;
        while ((dirent = readdir(dir)) != nullptr) {
            if (strncmp(dirent->d_name, "cpu", 3) == 0) {
                long coreNum;
                if (stringToLong(&coreNum, dirent->d_name + 3, 10) && (coreNum >= maxCoreNum)) {
                    maxCoreNum = coreNum + 1;
                }
            }
        }
        closedir(dir);

        if (maxCoreNum < 1) {
            LOG_ERROR("Unable to determine the number of cores on the target, no cpu# directories found");
            handleException();
        }

        return maxCoreNum;
    }

    static void setImplementer(int & cpuId, const int implementer)
    {
        if (cpuId == -1) {
            cpuId = 0;
        }
        cpuId |= implementer << 12;
    }

    static void setPart(int & cpuId, const int part)
    {
        if (cpuId == -1) {
            cpuId = 0;
        }
        cpuId |= part;
    }

    static const char HARDWARE[] = "Hardware";
    static const char CPU_IMPLEMENTER[] = "CPU implementer";
    static const char CPU_PART[] = "CPU part";
    static const char PROCESSOR[] = "processor";

    static constexpr unsigned makeCpuId(std::uint64_t midr)
    {
        return ((midr & 0xff000000) >> 12) | ((midr & 0xfff0) >> 4);
    }

    static std::string parseProcCpuInfo(bool justGetHardwareName, lib::Span<int> cpuIds)
    {
        std::string hardwareName;
        char temp[256]; // arbitrarily large amount

        FILE * f = lib::fopen_cloexec("/proc/cpuinfo", "r");
        if (f == nullptr) {
            LOG_DEBUG("Error opening /proc/cpuinfo\n"
                      "The core name in the captured xml file will be 'unknown'.");
            return hardwareName;
        }

        bool foundCoreName = false;
        constexpr size_t UNKNOWN_PROCESSOR = -1;
        size_t processor = UNKNOWN_PROCESSOR;
        size_t minProcessor = cpuIds.size();
        size_t maxProcessor = 0;
        bool foundProcessorInSection = false;
        int outOfPlaceCpuId = -1;
        bool invalidFormat = false;
        while (fgets(temp, sizeof(temp), f) != nullptr) {
            const size_t len = strlen(temp);

            if (len > 0) {
                // Replace the line feed with a null
                temp[len - 1] = '\0';
            }

            LOG_DEBUG("cpuinfo: %s", temp);

            if (len == 1) {
                // New section, clear the processor. Streamline will not know the cpus if the pre Linux 3.8 format of cpuinfo is encountered but also that no incorrect information will be transmitted.
                processor = UNKNOWN_PROCESSOR;
                foundProcessorInSection = false;
                continue;
            }

            const bool foundHardware = !foundCoreName && strncmp(temp, HARDWARE, sizeof(HARDWARE) - 1) == 0;
            const bool foundCPUImplementer = strncmp(temp, CPU_IMPLEMENTER, sizeof(CPU_IMPLEMENTER) - 1) == 0;
            const bool foundCPUPart = strncmp(temp, CPU_PART, sizeof(CPU_PART) - 1) == 0;
            const bool foundProcessor = strncmp(temp, PROCESSOR, sizeof(PROCESSOR) - 1) == 0;
            if (foundHardware || foundCPUImplementer || foundCPUPart || foundProcessor) {
                char * position = strchr(temp, ':');
                if (position == nullptr || static_cast<unsigned int>(position - temp) + 2 >= strlen(temp)) {
                    LOG_DEBUG("Unknown format of /proc/cpuinfo\n"
                              "The core name in the captured xml file will be 'unknown'.");
                    return hardwareName;
                }
                position += 2;

                if (foundHardware) {
                    hardwareName = position;
                    if (justGetHardwareName) {
                        return hardwareName;
                    }
                    foundCoreName = true;
                }

                if (foundCPUImplementer) {
                    int implementer;
                    if (!stringToInt(&implementer, position, 0)) {
                        // Do nothing
                    }
                    else if (processor != UNKNOWN_PROCESSOR) {
                        setImplementer(cpuIds[processor], implementer);
                    }
                    else {
                        setImplementer(outOfPlaceCpuId, implementer);
                        invalidFormat = true;
                    }
                }

                if (foundCPUPart) {
                    int cpuId;
                    if (!stringToInt(&cpuId, position, 0)) {
                        // Do nothing
                    }
                    else if (processor != UNKNOWN_PROCESSOR) {
                        setPart(cpuIds[processor], cpuId);
                    }
                    else {
                        setPart(outOfPlaceCpuId, cpuId);
                        invalidFormat = true;
                    }
                }

                if (foundProcessor) {
                    int processorId = -1;
                    const bool converted = stringToInt(&processorId, position, 0);

                    // update min and max processor ids
                    if (converted) {
                        minProcessor = (static_cast<size_t>(processorId) < minProcessor ? processorId : minProcessor);
                        maxProcessor = (static_cast<size_t>(processorId) > maxProcessor ? processorId : maxProcessor);
                    }

                    if (foundProcessorInSection) {
                        // Found a second processor in this section, ignore them all
                        processor = UNKNOWN_PROCESSOR;
                        invalidFormat = true;
                    }
                    else if (converted) {
                        processor = processorId;
                        if (processor >= cpuIds.size()) {
                            LOG_ERROR("Found processor %zu but max is %zu", processor, cpuIds.size());
                            handleException();
                        }
                        foundProcessorInSection = true;
                    }
                }
            }
        }
        fclose(f);

        if (invalidFormat && (outOfPlaceCpuId != -1) && (minProcessor <= maxProcessor)) {
            minProcessor = (minProcessor > 0 ? minProcessor : 0);
            maxProcessor = (maxProcessor < cpuIds.size() ? maxProcessor + 1 : cpuIds.size());

            for (size_t processor = minProcessor; processor < maxProcessor; ++processor) {
                if (cpuIds[processor] == -1) {
                    LOG_DEBUG("Setting global CPUID 0x%x for processors %zu ", outOfPlaceCpuId, processor);
                    cpuIds[processor] = outOfPlaceCpuId;
                }
            }
        }

        if (!foundCoreName) {
            LOG_DEBUG("Could not determine core name from /proc/cpuinfo\n"
                      "The core name in the captured xml file will be 'unknown'.");
        }

        return hardwareName;
    }

    std::string readCpuInfo(bool ignoreOffline, bool wantsHardwareName, lib::Span<int> cpuIds)
    {
        std::map<unsigned, unsigned> cpuToCluster;
        std::map<unsigned, std::set<unsigned>> clusterToCpuIds;
        std::map<unsigned, unsigned> cpuToCpuIds;

        // first collect the detailed state using the identifier if available
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::size_t identificationThreadCallbackCounter = 0;
            std::map<unsigned, PerCoreIdentificationThread::properties_t> collected_properties {};
            std::vector<std::unique_ptr<PerCoreIdentificationThread>> perCoreThreads {};

            // wake all cores; this ensures the contents of /proc/cpuinfo reflect the full range of cores in the system.
            // this works as follows:
            // - spawn one thread per core that is affined to each core
            // - once all cores are online and affined, *and* have read the data they are required to read, then they callback here to notify this method to continue
            // - the threads remain online until this function finishes (they are disposed of / terminated by destructor); this is so as
            //   to ensure that the cores remain online until cpuinfo is read
            if (!ignoreOffline) {
                for (unsigned cpu = 0; cpu < cpuIds.size(); ++cpu) {
                    perCoreThreads.emplace_back(new PerCoreIdentificationThread(
                        false,
                        cpu,
                        [&](unsigned c, PerCoreIdentificationThread::properties_t && properties) -> void {
                            std::lock_guard<std::mutex> guard {mutex};

                            // store it for later processing
                            collected_properties.emplace(c, std::move(properties));

                            // update completed count
                            identificationThreadCallbackCounter += 1;
                            cv.notify_one();
                        }));
                }

                // wait until all threads are online
                std::unique_lock<std::mutex> lock {mutex};
                auto succeeded = cv.wait_for(lock, std::chrono::seconds(10), [&] {
                    return identificationThreadCallbackCounter >= cpuIds.size();
                });
                if (!succeeded) {
                    LOG_DEBUG("Could not identify all CPU cores within the timeout period. Activated %zu of %zu",
                              identificationThreadCallbackCounter,
                              cpuIds.size());
                }
            }
            //
            // when we don't care about onlining the cores, just read them directly, one by one, any that are offline will be ignored anyway
            //
            else {
                for (unsigned cpu = 0; cpu < cpuIds.size(); ++cpu) {
                    collected_properties.emplace(cpu, PerCoreIdentificationThread::detectFor(cpu));
                }
            }

            // lock to prevent concurrent access to maps if one of the threads stalls
            std::lock_guard<std::mutex> lock(mutex);

            // process the collected properties
            for (auto const & entry : collected_properties) {
                auto c = entry.first;
                auto const & properties = entry.second;

                const unsigned cpuId = makeCpuId(properties.midr_el1);

                // store the cluster / core mappings to allow us to fill in any gaps by assuming the same core type per cluster
                if (properties.physical_package_id != PerCoreIdentificationThread::INVALID_PACKAGE_ID) {
                    cpuToCluster[c] = properties.physical_package_id;

                    // also map cluster to MIDR value if read
                    if (properties.midr_el1 != PerCoreIdentificationThread::INVALID_MIDR_EL1) {
                        clusterToCpuIds[properties.physical_package_id].insert(cpuId);
                    }

                    for (int sibling : properties.core_siblings) {
                        const unsigned sibling_cpu = sibling;

                        if (cpuToCluster.count(sibling_cpu) == 0) {
                            cpuToCluster[sibling_cpu] = properties.physical_package_id;
                        }
                    }
                }

                // map cpu to MIDR value if read
                if (properties.midr_el1 != PerCoreIdentificationThread::INVALID_MIDR_EL1) {
                    cpuToCpuIds[c] = cpuId;
                }
            }
        }

        // log what we learnt
        for (const auto & pair : cpuToCpuIds) {
            LOG_DEBUG("Read CPU %u CPUID from MIDR_EL1 -> 0x%05x", pair.first, pair.second);
        }
        for (const auto & pair : cpuToCluster) {
            LOG_DEBUG("Read CPU %u CLUSTER %u", pair.first, pair.second);
        }
        for (const auto & pair : clusterToCpuIds) {
            LOG_DEBUG("Read CLUSTER %u CPUIDs:", pair.first);
            for (auto cpuId : pair.second) {
                LOG_DEBUG("    0x%05x", cpuId);
            }
        }

        // did we successfully read all MIDR values from all cores?
        const bool knowAllMidrValues = (cpuToCpuIds.size() == cpuIds.size());

        // do we need to read /proc/cpuinfo
        std::string hardwareName = (wantsHardwareName || !knowAllMidrValues
                                        ? parseProcCpuInfo(/* justGetHardwareName = */ knowAllMidrValues, cpuIds)
                                        : "");

        // update/set known items from MIDR map and topology information. This will override anything read from /proc/cpuinfo
        updateCpuIdsFromTopologyInformation(cpuIds, cpuToCpuIds, cpuToCluster, clusterToCpuIds);

        return hardwareName;
    }
}
