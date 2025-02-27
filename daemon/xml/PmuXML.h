/* Copyright (C) 2010-2022 by Arm Limited. All rights reserved. */

#ifndef PMUXML_H
#define PMUXML_H

#include <set>
#include <string>
#include <string_view>
#include <vector>

class GatorCpu {
public:
    GatorCpu(std::string coreName,
             std::string id,
             std::string counterSet,
             const char * dtName,
             const char * speName,
             std::set<int> const & cpuIds,
             int pmncCounters,
             bool isV8);

    GatorCpu(std::string coreName,
             std::string id,
             std::string counterSet,
             std::string dtName,
             std::string speName,
             std::vector<int> cpuIds,
             int pmncCounters,
             bool isV8)
        : mCoreName(std::move(coreName)),
          mId(std::move(id)),
          mCounterSet(std::move(counterSet)),
          mDtName(std::move(dtName)),
          mSpeName(std::move(speName)),
          mCpuIds(std::move(cpuIds)),
          mPmncCounters(pmncCounters),
          mIsV8(isV8)
    {
    }

    GatorCpu(const GatorCpu & that, const char * speName)
        : mCoreName(that.mCoreName),
          mId(that.mId),
          mCounterSet(that.mCounterSet),
          mDtName(that.mDtName),
          mSpeName(speName),
          mCpuIds(that.mCpuIds),
          mPmncCounters(that.mPmncCounters),
          mIsV8(that.mIsV8)
    {
    }

    GatorCpu(const GatorCpu &) = default;
    GatorCpu & operator=(const GatorCpu &) = default;
    GatorCpu(GatorCpu &&) = default;
    GatorCpu & operator=(GatorCpu &&) = default;

    const char * getCoreName() const { return mCoreName.c_str(); }

    const char * getId() const { return mId.c_str(); }

    const char * getCounterSet() const { return mCounterSet.c_str(); }

    const char * getDtName() const { return (!mDtName.empty() ? mDtName.c_str() : nullptr); }

    const char * getSpeName() const { return (!mSpeName.empty() ? mSpeName.c_str() : nullptr); }

    bool getIsV8() const { return mIsV8; }

    const std::vector<int> & getCpuIds() const { return mCpuIds; }

    int getMinCpuId() const { return mCpuIds.front(); }

    int getMaxCpuId() const { return mCpuIds.back(); }

    int getPmncCounters() const { return mPmncCounters; }

    bool hasCpuId(int cpuId) const;

private:
    std::string mCoreName;
    std::string mId;
    std::string mCounterSet;
    std::string mDtName;
    std::string mSpeName;
    std::vector<int> mCpuIds;
    int mPmncCounters;
    bool mIsV8;
};

bool operator==(const GatorCpu & a, const GatorCpu & b);
bool operator<(const GatorCpu & a, const GatorCpu & b);

inline bool operator!=(const GatorCpu & a, const GatorCpu & b)
{
    return !(a == b);
}

class UncorePmu {
public:
    UncorePmu(std::string coreName,
              std::string id,
              std::string counterSet,
              std::string deviceInstance,
              int pmncCounters,
              bool hasCyclesCounter);

    UncorePmu(const UncorePmu &) = default;
    UncorePmu & operator=(const UncorePmu &) = default;
    UncorePmu(UncorePmu &&) = default;
    UncorePmu & operator=(UncorePmu &&) = default;

    const char * getCoreName() const { return mCoreName.c_str(); }

    const char * getId() const { return mId.c_str(); }

    const char * getCounterSet() const { return mCounterSet.c_str(); }

    /** Returns null if the uncore is not instanced */
    const char * getDeviceInstance() const { return (mDeviceInstance.empty() ? nullptr : mDeviceInstance.c_str()); }

    int getPmncCounters() const { return mPmncCounters; }

    bool getHasCyclesCounter() const { return mHasCyclesCounter; }

private:
    std::string mCoreName;
    std::string mId;
    std::string mCounterSet;
    std::string mDeviceInstance;
    int mPmncCounters;
    bool mHasCyclesCounter;
};

struct PmuXML {
    static const std::string_view DEFAULT_XML;

    const GatorCpu * findCpuByName(const char * name) const;
    const GatorCpu * findCpuById(int cpuid) const;
    const UncorePmu * findUncoreByName(const char * name) const;

    std::vector<GatorCpu> cpus;
    std::vector<UncorePmu> uncores;
};

#endif // PMUXML_H
