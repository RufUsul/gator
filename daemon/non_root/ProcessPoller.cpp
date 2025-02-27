/* Copyright (C) 2017-2021 by Arm Limited. All rights reserved. */

#include "non_root/ProcessPoller.h"

namespace non_root {
    namespace {
        class ProcessStateTrackerActiveScanIProcessPollerReceiver
            : public lnx::ProcessPollerBase::IProcessPollerReceiver {
        public:
            ProcessStateTrackerActiveScanIProcessPollerReceiver(ProcessStateTracker::ActiveScan & activeScan_)
                : activeScan(activeScan_)
            {
            }

            void onThreadDetails(int pid,
                                 int tid,
                                 const lnx::ProcPidStatFileRecord & statRecord,
                                 const std::optional<lnx::ProcPidStatmFileRecord> & statmRecord,
                                 const std::optional<lib::FsEntry> & exe) override
            {
                activeScan.addProcess(pid, tid, statRecord, statmRecord, exe);
            }

        private:
            ProcessStateTracker::ActiveScan & activeScan;
        };
    }

    ProcessPoller::ProcessPoller(ProcessStateTracker & processStateTracker_, lib::TimestampSource & timestampSource_)
        : processStateTracker(processStateTracker_), timestampSource(timestampSource_)
    {
    }

    void ProcessPoller::poll()
    {
        // get a new scan object from the process state tracker; this is how we check if a process was terminated
        // between one scan to the next
        auto processScan = processStateTracker.beginScan(timestampSource.getTimestampNS());
        ProcessStateTrackerActiveScanIProcessPollerReceiver receiver(*processScan);
        ProcessPollerBase::poll(true, true, receiver);
    }
}
