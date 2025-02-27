/* Copyright (C) 2013-2021 by Arm Limited. All rights reserved. */

#define BUFFER_USE_SESSION_DATA

#include "PerfAttrsBuffer.h"

#include "BufferUtils.h"
#include "Logging.h"
#include "SessionData.h"
#include "k/perf_event.h"

#include <cstring>

PerfAttrsBuffer::PerfAttrsBuffer(const int size, sem_t & readerSem) : buffer(size, readerSem)
{
    // fresh buffer will always have room for header
    // so no need to check space
    buffer.beginFrame(FrameType::PERF_ATTRS);
    buffer.packInt(0); // core (ignored)
}

void PerfAttrsBuffer::write(ISender & sender)
{
    buffer.write(sender);
}

int PerfAttrsBuffer::bytesAvailable() const
{
    return buffer.bytesAvailable();
}

void PerfAttrsBuffer::flush()
{
    buffer.endFrame();
    buffer.flush();

    buffer.waitForSpace(IRawFrameBuilder::MAX_FRAME_HEADER_SIZE + buffer_utils::MAXSIZE_PACK32);
    buffer.beginFrame(FrameType::PERF_ATTRS);
    buffer.packInt(0); // core (ignored)
}

void PerfAttrsBuffer::waitForSpace(int bytes)
{
    if (buffer.bytesAvailable() < bytes) {
        flush();
    }
    buffer.waitForSpace(bytes);
}

void PerfAttrsBuffer::marshalPea(const struct perf_event_attr * const pea, int key)
{
    waitForSpace(2 * buffer_utils::MAXSIZE_PACK32 + pea->size);
    buffer.packInt(static_cast<int32_t>(CodeType::PEA));
    buffer.writeBytes(pea, pea->size);
    buffer.packInt(key);
}

void PerfAttrsBuffer::marshalKeys(const int count, const uint64_t * const ids, const int * const keys)
{
    waitForSpace(2 * buffer_utils::MAXSIZE_PACK32
                 + count * (buffer_utils::MAXSIZE_PACK32 + buffer_utils::MAXSIZE_PACK64));
    buffer.packInt(static_cast<int32_t>(CodeType::KEYS));
    buffer.packInt(count);
    for (int i = 0; i < count; ++i) {
        buffer.packInt64(ids[i]);
        buffer.packInt(keys[i]);
    }
}

void PerfAttrsBuffer::marshalKeysOld(const int keyCount,
                                     const int * const keys,
                                     const int bytes,
                                     const char * const buf)
{
    waitForSpace((2 + keyCount) * buffer_utils::MAXSIZE_PACK32 + bytes);
    buffer.packInt(static_cast<int32_t>(CodeType::KEYS_OLD));
    buffer.packInt(keyCount);
    for (int i = 0; i < keyCount; ++i) {
        buffer.packInt(keys[i]);
    }
    buffer.writeBytes(buf, bytes);
}

void PerfAttrsBuffer::marshalFormat(const int length, const char * const format)
{
    waitForSpace(buffer_utils::MAXSIZE_PACK32 + length + 1);
    buffer.packInt(static_cast<int32_t>(CodeType::FORMAT));
    buffer.writeBytes(format, length + 1);
}

void PerfAttrsBuffer::marshalMaps(const int pid, const int tid, const char * const maps)
{
    const int mapsLen = strlen(maps) + 1;
    const int requiredLen = 3 * buffer_utils::MAXSIZE_PACK32 + mapsLen;

    // ignore map files that are *really* large
    if (!buffer.supportsWriteOfSize(requiredLen)) {
        LOG_WARNING("proc maps file too large for buffer (%d > %d bytes), ignoring", requiredLen, buffer.size());
        return;
    }

    waitForSpace(requiredLen);
    buffer.packInt(static_cast<int32_t>(CodeType::MAPS));
    buffer.packInt(pid);
    buffer.packInt(tid);
    buffer.writeBytes(maps, mapsLen);
}

void PerfAttrsBuffer::marshalComm(const int pid, const int tid, const char * const image, const char * const comm)
{
    const int imageLen = strlen(image) + 1;
    const int commLen = strlen(comm) + 1;
    waitForSpace(3 * buffer_utils::MAXSIZE_PACK32 + imageLen + commLen);
    buffer.packInt(static_cast<int32_t>(CodeType::COMM));
    buffer.packInt(pid);
    buffer.packInt(tid);
    buffer.writeBytes(image, imageLen);
    buffer.writeBytes(comm, commLen);
}

void PerfAttrsBuffer::onlineCPU(const uint64_t time, const int cpu)
{
    waitForSpace(buffer_utils::MAXSIZE_PACK32 + buffer_utils::MAXSIZE_PACK64);
    buffer.packInt(static_cast<int32_t>(CodeType::ONLINE_CPU));
    buffer.packInt64(time);
    buffer.packInt(cpu);
}

void PerfAttrsBuffer::offlineCPU(const uint64_t time, const int cpu)
{
    waitForSpace(buffer_utils::MAXSIZE_PACK32 + buffer_utils::MAXSIZE_PACK64);
    buffer.packInt(static_cast<int32_t>(CodeType::OFFLINE_CPU));
    buffer.packInt64(time);
    buffer.packInt(cpu);
}

void PerfAttrsBuffer::marshalKallsyms(const char * const kallsyms)
{
    const int kallsymsLen = strlen(kallsyms) + 1;
    const int requiredLen = 3 * buffer_utils::MAXSIZE_PACK32 + kallsymsLen;

    // ignore kallsyms files that are *really* large
    if (!buffer.supportsWriteOfSize(requiredLen)) {
        LOG_WARNING("kallsyms file too large for buffer (%d > %d bytes), ignoring", requiredLen, buffer.size());
        return;
    }

    waitForSpace(requiredLen);
    buffer.packInt(static_cast<int32_t>(CodeType::KALLSYMS));
    buffer.writeBytes(kallsyms, kallsymsLen);
}

void PerfAttrsBuffer::perfCounterHeader(const uint64_t time, const int numberOfCounters)
{
    // @formatter:off
    waitForSpace(
        // header (this function)
        buffer_utils::MAXSIZE_PACK32   // code type
        + buffer_utils::MAXSIZE_PACK64 // time
        // counters (perfCounter)
        + numberOfCounters
              * (buffer_utils::MAXSIZE_PACK32    // core
                 + buffer_utils::MAXSIZE_PACK32  // key
                 + buffer_utils::MAXSIZE_PACK64) // value
        // footer (perfCounterFooter)
        + buffer_utils::MAXSIZE_PACK32 // sentinel value
    );
    // @formatter:on
    buffer.packInt(static_cast<int32_t>(CodeType::COUNTERS));
    buffer.packInt64(time);
}

void PerfAttrsBuffer::perfCounter(const int core, const int key, const int64_t value)
{
    buffer.packInt(core);
    buffer.packInt(key);
    buffer.packInt64(value);
}

void PerfAttrsBuffer::perfCounterFooter()
{
    buffer.packInt(-1);
}

void PerfAttrsBuffer::marshalHeaderPage(const char * const headerPage)
{
    const int headerPageLen = strlen(headerPage) + 1;
    waitForSpace(buffer_utils::MAXSIZE_PACK32 + headerPageLen);
    buffer.packInt(static_cast<int32_t>(CodeType::HEADER_PAGE));
    buffer.writeBytes(headerPage, headerPageLen);
}

void PerfAttrsBuffer::marshalHeaderEvent(const char * const headerEvent)
{
    const int headerEventLen = strlen(headerEvent) + 1;
    waitForSpace(buffer_utils::MAXSIZE_PACK32 + headerEventLen);
    buffer.packInt(static_cast<int32_t>(CodeType::HEADER_EVENT));
    buffer.writeBytes(headerEvent, headerEventLen);
}
