//  Copyright (c) 2024 ffms2 contributors
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#ifndef FFMS_ROBUST_H
#define FFMS_ROBUST_H

#include "ffms.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavformat/avio.h>
}

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

struct AVFormatContext;

namespace robust {

int64_t SteadyNowNs();
int64_t UnixNowMs();

std::string DefaultCheckpointPath(const char *SourceFile, int Kind);
std::string DefaultReportPath(const char *SourceFile, int Kind, const std::string &CheckpointPath);
bool WriteTextFileAtomic(const std::string &Path, const std::string &Contents, std::string *Error);
bool ReadTextFile(const std::string &Path, std::string &Contents, std::string *Error);

struct Settings {
    bool StallDetection = false;
    int64_t StallTimeoutMs = 30000;
    bool Checkpoint = false;
    int64_t CheckpointIntervalMs = 30000;
    std::string CheckpointFile;
    std::string ReportFile;
    TStallCallback StallCallback = nullptr;
    void *StallPrivate = nullptr;

    static Settings FromOptions(const FFMS_RobustOptions *Options, const char *SourceFile, int Kind);
};

struct CheckpointRecord {
    int Version = 1;
    int Kind = FFMS_CHECKPOINT_UNKNOWN;
    int Status = FFMS_CHECKPOINT_RUNNING;
    int Track = -1;
    bool Finalized = false;
    int64_t LastFrame = -1;
    int64_t LastPTS = AV_NOPTS_VALUE;
    int64_t LastTimestampMs = -1;
    int64_t LastFilePosition = -1;
    int64_t UpdatedAtUnixMs = 0;
    std::string Source;
    int64_t SourceSize = -1;
    std::string Reason;
    std::map<std::string, std::string> Extra;
    std::string Body;
};

bool WriteCheckpoint(const std::string &Path, const CheckpointRecord &Record, std::string *Error);
bool ReadCheckpoint(const std::string &Path, CheckpointRecord &Record, std::string *Error);
void FillPublicCheckpointInfo(const CheckpointRecord &Record, FFMS_CheckpointInfo *Info);
void WriteFailureReport(const Settings &Options, const std::string &Source,
    const char *Phase, int Status, const std::string &Reason, int Track,
    int64_t Frame, int64_t PTS, int64_t TimestampMs, int64_t FilePosition);

class StallMonitor {
public:
    StallMonitor(const Settings &Options, const char *SourceFile);
    ~StallMonitor();

    StallMonitor(const StallMonitor &) = delete;
    StallMonitor &operator=(const StallMonitor &) = delete;

    void Start();
    void Stop();
    void Install(AVFormatContext *Context);

    bool Enabled() const { return Options.StallDetection; }
    bool Aborted() const { return AbortedFlag.load(std::memory_order_relaxed); }
    const std::string &AbortMessage() const { return Message; }
    const AVIOInterruptCB *GetInterruptCallback() const;

    void SetPhase(int Phase, const char *Detail);
    void SetPosition(int Track, int64_t Frame, int64_t PTS, int64_t TimestampMs, int64_t FilePosition);
    void Touch();
    void Check(const char *Action);

private:
    Settings Options;
    std::string Source;
    std::string ReportPath;

    std::atomic<bool> Started{false};
    std::atomic<bool> StopRequested{false};
    std::atomic<bool> AbortedFlag{false};
    std::atomic<int64_t> LastProgressNs{0};
    std::atomic<int> CurrentPhase{FFMS_ACTIVITY_IDLE};
    std::atomic<int> CurrentTrack{-1};
    std::atomic<int64_t> CurrentFrame{-1};
    std::atomic<int64_t> CurrentPTS{AV_NOPTS_VALUE};
    std::atomic<int64_t> CurrentTimestampMs{-1};
    std::atomic<int64_t> CurrentFilePosition{-1};
    std::atomic<const char *> CurrentDetail{nullptr};

    mutable std::mutex StateMutex;
    std::string Message;
    std::condition_variable StopCondition;
    std::mutex StopMutex;
    std::thread Watchdog;
    AVIOInterruptCB Interrupt = {};

    void Run();
    void TriggerStall(int64_t NoProgressMs);
    static int InterruptCallback(void *Opaque);
};

const char *ActivityPhaseName(int Phase);

} // namespace robust

#endif
