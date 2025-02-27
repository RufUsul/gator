/* Copyright (C) 2018-2022 by Arm Limited. All rights reserved. */

#include "linux/perf/PerfEventGroup.h"

#include "DynBuf.h"
#include "Logging.h"
#include "SessionData.h"
#include "k/perf_event.h"
#include "lib/Format.h"
#include "lib/Syscall.h"
#include "linux/perf/PerfUtils.h"
#include "xml/PmuXML.h"

#include <cassert>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <iostream>
#include <optional>
#include <set>

#include <fcntl.h>
#include <sys/ioctl.h>

namespace {
    constexpr unsigned long NANO_SECONDS_IN_ONE_SECOND = 1000000000UL;
    constexpr unsigned long NANO_SECONDS_IN_100_MS = 100000000UL;
    constexpr std::uint32_t MAX_SPE_WATERMARK = 2048U * 1024U;
    constexpr std::uint32_t MIN_SPE_WATERMARK = 4096U;

    /**
     * Dynamically adjust the aux_matermark value based on the sample frequency so that
     * we collect data every 1/nth of a second, applying some sensible limits with respect
     * to data size / processing cost on Streamline side.
     *
     * @param mmap_size The size of the aux mmap
     * @param count The sampling frequency
     * @return The aux_watermark value
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    constexpr std::uint32_t calculate_aux_watermark(std::size_t mmap_size, std::uint64_t count)
    {
        constexpr std::uint64_t fraction_of_second = 10; // 1/10s

        auto const frequency = std::max<std::uint64_t>(NANO_SECONDS_IN_ONE_SECOND / count, 1);
        auto const bps = (24 * frequency); // assume an average of 24 bytes per sample

        // wake up after ~(1/fraction) seconds worth of data, or 50% of buffer is full
        auto const pref_watermark = std::min<std::uint64_t>(mmap_size / 2, bps / fraction_of_second);

        // but ensure that the watermark is not too large as may be the case with high sample rate and large buffer
        // as this can be problematic for Streamline in system-wide mode
        return std::max<std::uint32_t>(std::min<std::uint64_t>(pref_watermark, MAX_SPE_WATERMARK), MIN_SPE_WATERMARK);
    }

    /**
     * Decode whether or no to set exclude_kernel (et al)
     *
     * @param type The attribute type
     * @param config The attribute config
     * @param exclude_requested Whether or not exclude was requested (either by perf_paranoid or by cli argument)
     * @return The exclude_kernel bit value
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    constexpr bool should_exclude_kernel(std::uint32_t type, std::uint64_t config, bool exclude_requested)
    {
        // don't need to exclude if it wasn't requested
        if (!exclude_requested) {
            return false;
        }

        // but should also not exclude for certain events
        if (type == PERF_TYPE_SOFTWARE) {
            return (config != PERF_COUNT_SW_CONTEXT_SWITCHES);
        }

        return true;
    }
}

bool perf_event_group_configurer_t::initEvent(perf_event_group_configurer_config_t & config,
                                              perf_event_t & event,
                                              bool is_header,
                                              bool requires_leader,
                                              PerfEventGroupIdentifier::Type type,
                                              const bool leader,
                                              attr_to_key_mapping_tracker_t & mapping_tracker,
                                              const int key,
                                              const IPerfGroups::Attr & attr,
                                              bool hasAuxData)
{
    event.attr.size = sizeof(event.attr);
    /* Emit time, read_format below, group leader id, and raw tracepoint info */
    const uint64_t sampleReadMask =
        (config.perfConfig.is_system_wide
             ? 0
             : PERF_SAMPLE_READ); // Unfortunately PERF_SAMPLE_READ is not allowed with inherit
    event.attr.sample_type =
        PERF_SAMPLE_TIME
        | (attr.sampleType & ~sampleReadMask)
        // required fields for reading 'id'
        | (config.perfConfig.has_sample_identifier ? PERF_SAMPLE_IDENTIFIER
                                                   : PERF_SAMPLE_TID | PERF_SAMPLE_IP | PERF_SAMPLE_ID)
        // see https://lkml.org/lkml/2012/7/18/355
        | (attr.type == PERF_TYPE_TRACEPOINT ? PERF_SAMPLE_PERIOD : 0)
        // always sample TID for application mode; we use it to attribute counter values to their processes
        | (config.perfConfig.is_system_wide && !attr.context_switch ? 0 : PERF_SAMPLE_TID)
        // must sample PERIOD is used if 'freq' to read the actual period value
        | (attr.freq ? PERF_SAMPLE_PERIOD : 0);

#if CONFIG_PERF_SUPPORT_REGISTER_UNWINDING
    // collect the user mode registers if sampling the callchain
    if (event.attr.sample_type & PERF_SAMPLE_CALLCHAIN) {
        event.attr.sample_type |= PERF_SAMPLE_REGS_USER;
        if (config.perfConfig.use_64bit_register_set) {
            // https://elixir.bootlin.com/linux/latest/source/arch/arm64/include/uapi/asm/perf_regs.h
            // bits 0-32 are set (PC = 2^32)
            event.attr.sample_regs_user = 0x1ffffffffull;
        }
        else {
            // https://elixir.bootlin.com/linux/latest/source/arch/arm/include/uapi/asm/perf_regs.h
            // bits 0-15 are set
            event.attr.sample_regs_user = 0xffffull;
        }
    }
#else
    event.attr.sample_regs_user = 0;
#endif

    // make sure all new children are counted too
    const bool use_inherit = !(config.perfConfig.is_system_wide || is_header);
    // group doesn't require a leader (so all events are stand alone)
    const bool every_attribute_in_own_group = use_inherit || (!requires_leader) || is_header;
    // use READ_FORMAT_GROUP; only when the leader and not a stand alone event
    const bool use_read_format_group = leader && (!use_inherit) && (!every_attribute_in_own_group) && (!is_header);

    // filter kernel events?
    const bool exclude_kernel = should_exclude_kernel(attr.type, attr.config, config.excludeKernelEvents);

    // when running in application mode, inherit must always be set, in system wide mode, inherit must always be clear
    event.attr.inherit = use_inherit;
    event.attr.inherit_stat = event.attr.inherit;
    /* Emit emit value in group format */
    // Unfortunately PERF_FORMAT_GROUP is not allowed with inherit
    event.attr.read_format = (use_read_format_group ? PERF_FORMAT_ID | PERF_FORMAT_GROUP //
                                                    : PERF_FORMAT_ID);
    // Always be on the CPU but only a perf_event_open group leader can be pinned
    // We can only use perf_event_open groups if PERF_FORMAT_GROUP is used to sample group members
    // If the group has no leader, then all members are in separate perf_event_open groups (and hence their own leader)
    event.attr.pinned = ((leader || every_attribute_in_own_group || is_header) ? 1 : 0);
    // group leader must start disabled, all others enabled
    event.attr.disabled = event.attr.pinned;
    /* have a sampling interrupt happen when we cross the wakeup_watermark boundary */
    event.attr.watermark = 1;
    /* Be conservative in flush size as only one buffer set is monitored */
    event.attr.wakeup_watermark = config.ringbuffer_config.data_buffer_size / 2;
    /* Use the monotonic raw clock if possible */
    event.attr.use_clockid = config.perfConfig.has_attr_clockid_support ? 1 : 0;
    event.attr.clockid = config.perfConfig.has_attr_clockid_support ? CLOCK_MONOTONIC_RAW : 0;
    event.attr.type = attr.type;
    event.attr.config = attr.config;
    event.attr.config1 = attr.config1;
    event.attr.config2 = attr.config2;
    event.attr.sample_period = attr.periodOrFreq;
    event.attr.mmap = attr.mmap;
    event.attr.comm = attr.comm;
    event.attr.comm_exec = attr.comm && config.perfConfig.has_attr_comm_exec;
    event.attr.freq = attr.freq;
    event.attr.task = attr.task;
    /* sample_id_all should always be set (or should always match pinned); it is required for any non-grouped event, for grouped events it is ignored for anything but the leader */
    event.attr.sample_id_all = 1;
    event.attr.context_switch = attr.context_switch;
    event.attr.exclude_kernel = (exclude_kernel ? 1 : 0);
    event.attr.exclude_hv = (exclude_kernel ? 1 : 0);
    event.attr.exclude_idle = (exclude_kernel ? 1 : 0);
    event.attr.exclude_callchain_kernel =
        (config.excludeKernelEvents && config.perfConfig.has_exclude_callchain_kernel ? 1 : 0);
    event.attr.aux_watermark = (hasAuxData ? calculate_aux_watermark(config.ringbuffer_config.aux_buffer_size, //
                                                                     event.attr.sample_period)                 //
                                           : 0);
    event.key = key;

    // [SDDAP-10625] - trace context switch information for SPE attributes.
    // it is required (particularly in system-wide mode) to be able to see
    // the boundarys of SPE data, as it is not guaranteed to get PERF_RECORD_ITRACE_START
    // between two processes if they are sampled by the same SPE attribute.
    if (type == PerfEventGroupIdentifier::Type::SPE) {
        if (!config.perfConfig.has_attr_context_switch) {
            LOG_ERROR("SPE requires context switch information");
            return false;
        }
        event.attr.context_switch = true;
    }

    // track the mapping from key->attr
    mapping_tracker(key, event.attr);

    return true;
}

bool perf_event_group_configurer_t::addEvent(const bool leader,
                                             attr_to_key_mapping_tracker_t & mapping_tracker,
                                             const int key,
                                             const IPerfGroups::Attr & attr,
                                             bool hasAuxData)
{
    if (leader && !state.events.empty()) {
        assert(false && "Cannot set leader for non-empty group");
        return false;
    }
    if (state.events.size() >= INT_MAX) {
        return false;
    }

    state.events.emplace_back();
    perf_event_t & event = state.events.back();

    return initEvent(config,
                     event,
                     false,
                     requiresLeader(),
                     identifier.getType(),
                     leader,
                     mapping_tracker,
                     key,
                     attr,
                     hasAuxData);
}

bool perf_event_group_configurer_t::createGroupLeader(attr_to_key_mapping_tracker_t & mapping_tracker)
{
    switch (identifier.getType()) {
        case PerfEventGroupIdentifier::Type::PER_CLUSTER_CPU:
            return createCpuGroupLeader(mapping_tracker);

        case PerfEventGroupIdentifier::Type::UNCORE_PMU:
            return createUncoreGroupLeader(mapping_tracker);

        case PerfEventGroupIdentifier::Type::SPECIFIC_CPU:
        case PerfEventGroupIdentifier::Type::GLOBAL:
        case PerfEventGroupIdentifier::Type::SPE:
        default:
            assert(false && "Should not be called");
            return false;
    }
}

bool perf_event_group_configurer_t::createCpuGroupLeader(attr_to_key_mapping_tracker_t & mapping_tracker)
{
    const bool enableCallChain = (config.backtraceDepth > 0);

    IPerfGroups::Attr attr {};
    attr.sampleType = PERF_SAMPLE_TID | PERF_SAMPLE_READ;
    attr.mmap = true;
    attr.comm = true;
    attr.task = true;
    bool enableTaskClock = false;

    // [SDDAP-10028] Do not use sched_switch in app tracing mode as it only triggers on switch-out (even when tracing as root)
    if (config.perfConfig.can_access_tracepoints && config.perfConfig.is_system_wide) {
        // Use sched switch to drive the sampling so that event counts are
        // exactly attributed to each thread in system-wide mode
        if (config.schedSwitchId == UNKNOWN_TRACEPOINT_ID) {
            LOG_DEBUG("Unable to read sched_switch id");
            return false;
        }
        attr.type = PERF_TYPE_TRACEPOINT;
        attr.config = config.schedSwitchId;
        attr.periodOrFreq = 1;
        // collect sched switch info from the tracepoint
        attr.sampleType |= PERF_SAMPLE_RAW;
    }
    else {
        attr.type = PERF_TYPE_SOFTWARE;
        if (config.perfConfig.has_attr_context_switch) {
            // collect sched switch info directly from perf
            attr.context_switch = true;

            // use dummy as leader if possible
            if (config.perfConfig.has_count_sw_dummy) {
                attr.config = PERF_COUNT_SW_DUMMY;
                attr.periodOrFreq = 0;
            }
            // otherwise use sampling as leader
            else {
                attr.config = PERF_COUNT_SW_CPU_CLOCK;
                attr.periodOrFreq = (config.sampleRate > 0 && config.enablePeriodicSampling
                                         ? NANO_SECONDS_IN_ONE_SECOND / config.sampleRate
                                         : 0);
                attr.sampleType |=
                    PERF_SAMPLE_TID | PERF_SAMPLE_IP | PERF_SAMPLE_READ | (enableCallChain ? PERF_SAMPLE_CALLCHAIN : 0);
            }
        }
        else if (!config.perfConfig.exclude_kernel) {
            // use context switches as leader. this should give us 'switch-out' events
            attr.config = PERF_COUNT_SW_CONTEXT_SWITCHES;
            attr.periodOrFreq = 1;
            attr.sampleType |= PERF_SAMPLE_TID;
            enableTaskClock = true;
        }
        else {
            // no context switches at all :-(
            attr.config = PERF_COUNT_SW_CPU_CLOCK;
            attr.periodOrFreq =
                (config.sampleRate > 0 && config.enablePeriodicSampling ? NANO_SECONDS_IN_ONE_SECOND / config.sampleRate
                                                                        : 0);
            attr.sampleType |=
                PERF_SAMPLE_TID | PERF_SAMPLE_IP | PERF_SAMPLE_READ | (enableCallChain ? PERF_SAMPLE_CALLCHAIN : 0);
        }
    }

    // Group leader
    if (!addEvent(true, mapping_tracker, config.schedSwitchKey, attr, false)) {
        return false;
    }

    // Periodic PC sampling
    if ((attr.config != PERF_COUNT_SW_CPU_CLOCK) && config.sampleRate > 0 && config.enablePeriodicSampling) {
        IPerfGroups::Attr pcAttr {};
        pcAttr.type = PERF_TYPE_SOFTWARE;
        pcAttr.config = PERF_COUNT_SW_CPU_CLOCK;
        pcAttr.sampleType =
            PERF_SAMPLE_TID | PERF_SAMPLE_IP | PERF_SAMPLE_READ | (enableCallChain ? PERF_SAMPLE_CALLCHAIN : 0);
        pcAttr.periodOrFreq = NANO_SECONDS_IN_ONE_SECOND / config.sampleRate;
        if (!addEvent(false, mapping_tracker, nextDummyKey(), pcAttr, false)) {
            return false;
        }
    }

    // use high frequency task clock to attempt to catch the first switch back to a process after a switch out
    // this should give us approximate 'switch-in' events
    if (enableTaskClock) {
        IPerfGroups::Attr taskClockAttr {};
        taskClockAttr.type = PERF_TYPE_SOFTWARE;
        taskClockAttr.config = PERF_COUNT_SW_TASK_CLOCK;
        taskClockAttr.periodOrFreq = 100000UL; // equivalent to 100us
        taskClockAttr.sampleType = PERF_SAMPLE_TID;
        if (!addEvent(false, mapping_tracker, nextDummyKey(), taskClockAttr, false)) {
            return false;
        }
    }

    return true;
}

bool perf_event_group_configurer_t::createUncoreGroupLeader(attr_to_key_mapping_tracker_t & mapping_tracker)
{
    IPerfGroups::Attr attr {};
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.sampleType = PERF_SAMPLE_READ;
    // Non-CPU PMUs are sampled every 100ms for Sample Rate: None otherwise they would never be sampled
    attr.periodOrFreq =
        (config.sampleRate > 0 ? NANO_SECONDS_IN_ONE_SECOND / config.sampleRate : NANO_SECONDS_IN_100_MS);

    return addEvent(true, mapping_tracker, nextDummyKey(), attr, false);
}

int perf_event_group_configurer_t::nextDummyKey(perf_event_group_configurer_config_t & config)
{
    return config.dummyKeyCounter--;
}
