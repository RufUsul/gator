/* Copyright (C) 2018-2021 by Arm Limited. All rights reserved. */

#include "lib/WaitForProcessPoller.h"

#include "Logging.h"

#include <string>

namespace {
    /**
     * Utility class that processes the results of a single pass
     */
    class WaitForProcessPollerPass : public lnx::ProcessPollerBase::IProcessPollerReceiver {
    public:
        WaitForProcessPollerPass(const std::string & commandName, const std::optional<lib::FsEntry> & realPath)
            : mCommandName(commandName), mRealPath(realPath)
        {
        }

        [[nodiscard]] inline const std::set<int> & pids() const { return mPids; }

        void onProcessDirectory(int pid, const lib::FsEntry & path) override
        {
            if (shouldTrack(path)) {
                trackPid(pid);
            }
        }

    private:
        std::set<int> mPids {};

        const std::string & mCommandName;
        const std::optional<lib::FsEntry> & mRealPath;

        [[nodiscard]] bool shouldTrack(const lib::FsEntry & path) const
        {
            if (!mCommandName.empty()) {
                const auto cmdlineFile = lib::FsEntry::create(path, "cmdline");
                const auto cmdline = lib::readFileContents(cmdlineFile);

                // cmdline is separated by nulls so use c_str() to extract the command name
                const std::string command {cmdline.c_str()}; // NOLINT(readability-redundant-string-cstr)
                if (!command.empty()) {
                    LOG_DEBUG("Wait for Process: Scanning '%s': cmdline[0] = '%s'",
                              path.path().c_str(),
                              command.c_str());

                    // track it if they are the same string
                    if (mCommandName == command) {
                        LOG_DEBUG("    Selected as cmdline matches");
                        return true;
                    }

                    // track it if they are the same file, or if they are the same basename
                    const auto commandPath = lib::FsEntry::create(command);
                    const auto realPath = commandPath.realpath();

                    // they are the same executable command
                    if (mRealPath && realPath && (*mRealPath == *realPath)) {
                        LOG_DEBUG("    Selected as realpath matches (%s)", mRealPath->path().c_str());
                        return true;
                    }

                    // the basename of the command matches the command name
                    // (e.g. /usr/bin/ls == ls)
                    if (commandPath.name() == mCommandName) {
                        LOG_DEBUG("    Selected as name matches");
                        return true;
                    }
                }
            }

            // check exe
            if (mRealPath) {
                const auto exeFile = lib::FsEntry::create(path, "exe");
                const auto realPath = exeFile.realpath();

                // they are the same executable command
                if (realPath && (*mRealPath == *realPath)) {
                    LOG_DEBUG("Wait for Process: Selected as exe matches (%s)", mRealPath->path().c_str());
                    return true;
                }
            }

            return false;
        }

        void trackPid(int pid) { mPids.insert(pid); }
    };
}

WaitForProcessPoller::WaitForProcessPoller(const char * commandName)
    : mCommandName(commandName), mRealPath(lib::FsEntry::create(commandName).realpath())
{
}

bool WaitForProcessPoller::poll(std::set<int> & pids)
{
    // poll
    WaitForProcessPollerPass pass {mCommandName, mRealPath};
    ProcessPollerBase::poll(false, false, pass);

    const auto & detectedPids = pass.pids();
    pids.insert(detectedPids.begin(), detectedPids.end());
    return !detectedPids.empty();
}
