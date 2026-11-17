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

#include "robust.h"

#include "utils.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace robust {

namespace {

const char *KindName(int Kind) {
    switch (Kind) {
    case FFMS_CHECKPOINT_INDEX: return "index";
    case FFMS_CHECKPOINT_DECODE: return "decode";
    case FFMS_CHECKPOINT_AUDIO: return "audio";
    default: return "unknown";
    }
}

int ParseKind(const std::string &Value) {
    if (Value == "index")
        return FFMS_CHECKPOINT_INDEX;
    if (Value == "decode")
        return FFMS_CHECKPOINT_DECODE;
    if (Value == "audio")
        return FFMS_CHECKPOINT_AUDIO;
    return FFMS_CHECKPOINT_UNKNOWN;
}

const char *StatusName(int Status) {
    switch (Status) {
    case FFMS_CHECKPOINT_RUNNING: return "running";
    case FFMS_CHECKPOINT_FAILED: return "failed";
    case FFMS_CHECKPOINT_STALLED: return "stalled";
    case FFMS_CHECKPOINT_CANCELLED: return "cancelled";
    case FFMS_CHECKPOINT_COMPLETED: return "completed";
    default: return "unknown";
    }
}

std::string Trim(const std::string &Value) {
    size_t Begin = 0;
    size_t End = Value.size();
    while (Begin < End && (Value[Begin] == ' ' || Value[Begin] == '\t' || Value[Begin] == '\r'))
        ++Begin;
    while (End > Begin && (Value[End - 1] == ' ' || Value[End - 1] == '\t' || Value[End - 1] == '\r'))
        --End;
    return Value.substr(Begin, End - Begin);
}

std::string EscapeValue(const std::string &Value) {
    std::string Out;
    Out.reserve(Value.size());
    for (size_t i = 0; i < Value.size(); ++i) {
        switch (Value[i]) {
        case '\\': Out += "\\\\"; break;
        case '\n': Out += "\\n"; break;
        case '\r': Out += "\\r"; break;
        default: Out += Value[i]; break;
        }
    }
    return Out;
}

std::string UnescapeValue(const std::string &Value) {
    std::string Out;
    Out.reserve(Value.size());
    for (size_t i = 0; i < Value.size(); ++i) {
        if (Value[i] == '\\' && i + 1 < Value.size()) {
            char Next = Value[++i];
            if (Next == 'n')
                Out += '\n';
            else if (Next == 'r')
                Out += '\r';
            else
                Out += Next;
        } else {
            Out += Value[i];
        }
    }
    return Out;
}

bool RenameReplace(const std::string &From, const std::string &To, std::string *Error) {
#ifdef _WIN32
    if (MoveFileExA(From.c_str(), To.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        if (Error)
            *Error = "MoveFileEx failed for '" + To + "'";
        return false;
    }
#else
    if (std::rename(From.c_str(), To.c_str()) != 0) {
        if (Error)
            *Error = "rename to '" + To + "' failed: " + std::strerror(errno);
        return false;
    }
#endif
    return true;
}

} // namespace

int64_t SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t UnixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string DefaultCheckpointPath(const char *SourceFile, int Kind) {
    const std::string Source = SourceFile ? SourceFile : "ffms2";
    if (Source.find("://") == std::string::npos)
        return Source + ".ffms2-" + KindName(Kind) + ".checkpoint.txt";

    uint64_t Hash = 1469598103934665603ULL;
    for (size_t i = 0; i < Source.size(); ++i) {
        Hash ^= static_cast<unsigned char>(Source[i]);
        Hash *= 1099511628211ULL;
    }
    size_t Slash = Source.find_last_of('/');
    std::string Base = Slash == std::string::npos ? "input" : Source.substr(Slash + 1);
    if (Base.empty())
        Base = "input";
    for (size_t i = 0; i < Base.size(); ++i) {
        const unsigned char C = static_cast<unsigned char>(Base[i]);
        if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
              (C >= '0' && C <= '9') || C == '.' || C == '_' || C == '-'))
            Base[i] = '_';
    }
    std::ostringstream Out;
    Out << "ffms2-" << Base << '-' << std::hex << Hash << '-' << KindName(Kind) << ".checkpoint.txt";
    return Out.str();
}

std::string DefaultReportPath(const char *SourceFile, int Kind, const std::string &CheckpointPath) {
    if (!CheckpointPath.empty())
        return CheckpointPath + ".report.txt";
    return std::string(SourceFile ? SourceFile : "ffms2") + ".ffms2-" + KindName(Kind) + ".stall-report.txt";
}

bool WriteTextFileAtomic(const std::string &Path, const std::string &Contents, std::string *Error) {
    if (Path.empty()) {
        if (Error)
            *Error = "empty file name";
        return false;
    }

    std::string Temp = Path + ".tmp";
#ifdef _WIN32
    Temp += "-" + std::to_string(GetCurrentProcessId());
#else
    Temp += "-" + std::to_string(getpid());
#endif

    {
        std::ofstream Out(Temp.c_str(), std::ios::binary | std::ios::trunc);
        if (!Out) {
            if (Error)
                *Error = "cannot open '" + Temp + "' for writing";
            return false;
        }
        Out.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
        Out.flush();
        if (!Out) {
            if (Error)
                *Error = "failed while writing '" + Temp + "'";
            Out.close();
            std::remove(Temp.c_str());
            return false;
        }
    }

    if (!RenameReplace(Temp, Path, Error)) {
        std::remove(Temp.c_str());
        return false;
    }
    return true;
}

bool ReadTextFile(const std::string &Path, std::string &Contents, std::string *Error) {
    std::ifstream In(Path.c_str(), std::ios::binary);
    if (!In) {
        if (Error)
            *Error = "cannot open '" + Path + "'";
        return false;
    }
    std::ostringstream Buffer;
    Buffer << In.rdbuf();
    if (!In.good() && !In.eof()) {
        if (Error)
            *Error = "failed while reading '" + Path + "'";
        return false;
    }
    Contents = Buffer.str();
    return true;
}

Settings Settings::FromOptions(const FFMS_RobustOptions *Options, const char *SourceFile, int Kind) {
    Settings S;
    if (!Options)
        return S;

    S.StallDetection = Options->EnableStallDetection != 0;
    S.Checkpoint = Options->EnableCheckpoint != 0;
    S.StallTimeoutMs = Options->StallTimeoutMs > 0 ? Options->StallTimeoutMs : 30000;
    S.CheckpointIntervalMs = Options->CheckpointIntervalMs > 0 ? Options->CheckpointIntervalMs : 30000;
    S.StallCallback = Options->StallCallback;
    S.StallPrivate = Options->StallPrivate;
    S.CheckpointFile = Options->CheckpointFile ? Options->CheckpointFile : "";
    S.ReportFile = Options->ReportFile ? Options->ReportFile : "";

    if (S.Checkpoint && S.CheckpointFile.empty())
        S.CheckpointFile = DefaultCheckpointPath(SourceFile, Kind);
    if ((S.StallDetection || S.Checkpoint) && S.ReportFile.empty())
        S.ReportFile = DefaultReportPath(SourceFile, Kind, S.Checkpoint ? S.CheckpointFile : "");

    return S;
}

bool WriteCheckpoint(const std::string &Path, const CheckpointRecord &Record, std::string *Error) {
    std::ostringstream Out;
    Out << "# FFMS2 checkpoint v" << Record.Version << "\n"
        << "# Plain text progress record; safe to inspect, copy, or repair.\n"
        << "format=ffms2-checkpoint\n"
        << "version=" << Record.Version << "\n"
        << "kind=" << KindName(Record.Kind) << "\n"
        << "status=" << Record.Status << "\n"
        << "status_name=" << StatusName(Record.Status) << "\n"
        << "finalized=" << (Record.Finalized ? 1 : 0) << "\n"
        << "track=" << Record.Track << "\n"
        << "last_frame=" << Record.LastFrame << "\n"
        << "last_pts=" << Record.LastPTS << "\n"
        << "last_timestamp_ms=" << Record.LastTimestampMs << "\n"
        << "last_file_position=" << Record.LastFilePosition << "\n"
        << "source=" << EscapeValue(Record.Source) << "\n"
        << "source_size=" << Record.SourceSize << "\n"
        << "reason=" << EscapeValue(Record.Reason) << "\n"
        << "updated_unix_ms=" << Record.UpdatedAtUnixMs << "\n";

    for (std::map<std::string, std::string>::const_iterator it = Record.Extra.begin(); it != Record.Extra.end(); ++it)
        Out << it->first << "=" << EscapeValue(it->second) << "\n";

    Out << "\n" << Record.Body;
    if (!Record.Body.empty() && Record.Body[Record.Body.size() - 1] != '\n')
        Out << "\n";

    return WriteTextFileAtomic(Path, Out.str(), Error);
}

bool ReadCheckpoint(const std::string &Path, CheckpointRecord &Record, std::string *Error) {
    std::string Contents;
    if (!ReadTextFile(Path, Contents, Error))
        return false;

    if (Contents.find("# FFMS2 checkpoint") != 0) {
        if (Error)
            *Error = "'" + Path + "' is not an FFMS2 text checkpoint";
        return false;
    }

    std::istringstream In(Contents);
    std::string Line;
    bool InBody = false;
    bool SawFormat = false;
    Record = CheckpointRecord();

    while (std::getline(In, Line)) {
        if (!Line.empty() && Line[Line.size() - 1] == '\r')
            Line.pop_back();

        if (!InBody) {
            if (Line.empty()) {
                InBody = true;
                continue;
            }
            if (Line[0] == '#')
                continue;

            size_t Eq = Line.find('=');
            if (Eq == std::string::npos)
                continue;
            std::string Key = Trim(Line.substr(0, Eq));
            std::string Value = UnescapeValue(Line.substr(Eq + 1));

            if (Key == "format") {
                SawFormat = Value == "ffms2-checkpoint";
            } else if (Key == "version") {
                Record.Version = std::atoi(Value.c_str());
            } else if (Key == "kind") {
                Record.Kind = ParseKind(Value);
            } else if (Key == "status") {
                Record.Status = std::atoi(Value.c_str());
            } else if (Key == "finalized") {
                Record.Finalized = std::atoi(Value.c_str()) != 0;
            } else if (Key == "track") {
                Record.Track = std::atoi(Value.c_str());
            } else if (Key == "last_frame") {
                Record.LastFrame = std::strtoll(Value.c_str(), nullptr, 10);
            } else if (Key == "last_pts") {
                Record.LastPTS = std::strtoll(Value.c_str(), nullptr, 10);
            } else if (Key == "last_timestamp_ms") {
                Record.LastTimestampMs = std::strtoll(Value.c_str(), nullptr, 10);
            } else if (Key == "last_file_position") {
                Record.LastFilePosition = std::strtoll(Value.c_str(), nullptr, 10);
            } else if (Key == "source") {
                Record.Source = Value;
            } else if (Key == "source_size") {
                Record.SourceSize = std::strtoll(Value.c_str(), nullptr, 10);
            } else if (Key == "reason") {
                Record.Reason = Value;
            } else if (Key == "updated_unix_ms") {
                Record.UpdatedAtUnixMs = std::strtoll(Value.c_str(), nullptr, 10);
            } else {
                Record.Extra[Key] = Value;
            }
        } else {
            Record.Body += Line;
            Record.Body += '\n';
        }
    }

    if (!SawFormat || Record.Version != 1) {
        if (Error)
            *Error = "'" + Path + "' has an unsupported checkpoint format or version";
        return false;
    }
    return true;
}

void FillPublicCheckpointInfo(const CheckpointRecord &Record, FFMS_CheckpointInfo *Info) {
    if (!Info)
        return;
    Info->Version = Record.Version;
    Info->Kind = Record.Kind;
    Info->Status = Record.Status;
    Info->Finalized = Record.Finalized ? 1 : 0;
    Info->Track = Record.Track;
    Info->LastFrame = Record.LastFrame;
    Info->LastPTS = Record.LastPTS;
    Info->LastTimestampMs = Record.LastTimestampMs;
    Info->LastFilePosition = Record.LastFilePosition;
    Info->SourceSize = Record.SourceSize;
    Info->UpdatedAtUnixMs = Record.UpdatedAtUnixMs;
    std::snprintf(Info->Reason, sizeof(Info->Reason), "%s", Record.Reason.c_str());
}

void WriteFailureReport(const Settings &Options, const std::string &Source,
    const char *Phase, int Status, const std::string &Reason, int Track,
    int64_t Frame, int64_t PTS, int64_t TimestampMs, int64_t FilePosition) {
    if (Options.ReportFile.empty())
        return;

    std::ostringstream Report;
    Report << "# FFMS2 failure report\n"
           << "format=ffms2-failure-report\n"
           << "version=1\n"
           << "source=" << EscapeValue(Source) << "\n"
           << "phase=" << (Phase ? Phase : "unknown") << "\n"
           << "status=" << Status << "\n"
           << "status_name=" << StatusName(Status) << "\n"
           << "reason=" << EscapeValue(Reason) << "\n"
           << "track=" << Track << "\n"
           << "last_frame=" << Frame << "\n"
           << "last_pts=" << PTS << "\n"
           << "last_timestamp_ms=" << TimestampMs << "\n"
           << "last_file_position=" << FilePosition << "\n"
           << "detected_unix_ms=" << UnixNowMs() << "\n";
    std::string Ignored;
    WriteTextFileAtomic(Options.ReportFile, Report.str(), &Ignored);
}

const char *ActivityPhaseName(int Phase) {
    switch (Phase) {
    case FFMS_ACTIVITY_OPENING: return "opening input";
    case FFMS_ACTIVITY_READING: return "reading the next packet";
    case FFMS_ACTIVITY_DECODING: return "decoding a frame";
    case FFMS_ACTIVITY_SEEKING: return "seeking";
    case FFMS_ACTIVITY_FINALIZING: return "finalizing the index";
    default: return "idle";
    }
}

StallMonitor::StallMonitor(const Settings &Options_, const char *SourceFile)
    : Options(Options_), Source(SourceFile ? SourceFile : ""), ReportPath(Options_.ReportFile) {
    LastProgressNs.store(SteadyNowNs(), std::memory_order_relaxed);
    Interrupt.callback = &StallMonitor::InterruptCallback;
    Interrupt.opaque = this;
}

StallMonitor::~StallMonitor() {
    Stop();
}

void StallMonitor::Start() {
    if (!Options.StallDetection || Started.exchange(true))
        return;
    LastProgressNs.store(SteadyNowNs(), std::memory_order_relaxed);
    Watchdog = std::thread(&StallMonitor::Run, this);
}

void StallMonitor::Stop() {
    if (!Started.load())
        return;
    {
        std::lock_guard<std::mutex> Lock(StopMutex);
        StopRequested.store(true);
    }
    StopCondition.notify_all();
    if (Watchdog.joinable())
        Watchdog.join();
    Started.store(false);
}

void StallMonitor::Install(AVFormatContext *Context) {
    if (!Context || !Options.StallDetection)
        return;
    Context->interrupt_callback = Interrupt;
}

const AVIOInterruptCB *StallMonitor::GetInterruptCallback() const {
    return Options.StallDetection ? &Interrupt : nullptr;
}

void StallMonitor::SetPhase(int Phase, const char *Detail) {
    if (!Options.StallDetection) {
        Touch();
        return;
    }
    CurrentPhase.store(Phase, std::memory_order_relaxed);
    CurrentDetail.store(Detail ? Detail : ActivityPhaseName(Phase), std::memory_order_relaxed);
    Touch();
}

void StallMonitor::SetPosition(int Track, int64_t Frame, int64_t PTS, int64_t TimestampMs, int64_t FilePosition) {
    CurrentTrack.store(Track, std::memory_order_relaxed);
    CurrentFrame.store(Frame, std::memory_order_relaxed);
    CurrentPTS.store(PTS, std::memory_order_relaxed);
    CurrentTimestampMs.store(TimestampMs, std::memory_order_relaxed);
    CurrentFilePosition.store(FilePosition, std::memory_order_relaxed);
}

void StallMonitor::Touch() {
    if (Options.StallDetection)
        LastProgressNs.store(SteadyNowNs(), std::memory_order_relaxed);
}

void StallMonitor::Check(const char *Action) {
    if (!AbortedFlag.load(std::memory_order_acquire))
        return;

    std::string MessageCopy;
    {
        std::lock_guard<std::mutex> Lock(StateMutex);
        MessageCopy = Message;
    }
    throw FFMS_Exception(FFMS_ERROR_STALLED, FFMS_ERROR_FILE_READ,
        std::string(Action ? Action : "Operation") + " aborted: " + MessageCopy);
}

void StallMonitor::Run() {
    const int64_t TimeoutNs = Options.StallTimeoutMs * 1000000;
    std::clock_t LastCpu = std::clock();
    int64_t LastCpuWallNs = SteadyNowNs();
    std::unique_lock<std::mutex> Lock(StopMutex);

    while (!StopRequested.load()) {
        StopCondition.wait_for(Lock, std::chrono::milliseconds(100));
        if (StopRequested.load() || AbortedFlag.load())
            break;

        int64_t Now = SteadyNowNs();
        int64_t Last = LastProgressNs.load(std::memory_order_relaxed);
        std::clock_t CpuNow = std::clock();
        int64_t CpuDeltaMs = static_cast<int64_t>(CpuNow - LastCpu) * 1000 / CLOCKS_PER_SEC;
        int64_t WallDeltaMs = (Now - LastCpuWallNs) / 1000000;
        LastCpu = CpuNow;
        LastCpuWallNs = Now;
        if (CpuDeltaMs >= 10 && CpuDeltaMs * 10 >= WallDeltaMs) {
            LastProgressNs.store(Now, std::memory_order_relaxed);
            continue;
        }
        if (Now - Last >= TimeoutNs) {
            Lock.unlock();
            TriggerStall((Now - Last) / 1000000);
            Lock.lock();
        }
    }
}

void StallMonitor::TriggerStall(int64_t NoProgressMs) {
    std::string Detail;
    {
        const char *DetailPtr = CurrentDetail.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> Lock(StateMutex);
        Detail = DetailPtr ? DetailPtr : ActivityPhaseName(CurrentPhase.load());
        std::ostringstream MessageStream;
        MessageStream << "no progress for " << NoProgressMs
                      << " ms while " << Detail
                      << " (last frame " << CurrentFrame.load()
                      << ", PTS " << CurrentPTS.load()
                      << ", timestamp " << CurrentTimestampMs.load() << " ms"
                      << ", file position " << CurrentFilePosition.load() << ")";
        Message = MessageStream.str();
    }

    FFMS_StallInfo Info = {};
    Info.Phase = CurrentPhase.load();
    Info.Track = CurrentTrack.load();
    Info.Frame = CurrentFrame.load();
    Info.PTS = CurrentPTS.load();
    Info.TimestampMs = CurrentTimestampMs.load();
    Info.FilePosition = CurrentFilePosition.load();
    Info.NoProgressMs = NoProgressMs;
    Info.Detail = Detail.c_str();

    if (!ReportPath.empty()) {
        std::ostringstream Report;
        Report << "# FFMS2 stall report\n"
               << "format=ffms2-stall-report\n"
               << "version=1\n"
               << "source=" << EscapeValue(Source) << "\n"
               << "phase=" << ActivityPhaseName(Info.Phase) << "\n"
               << "detail=" << EscapeValue(Detail) << "\n"
               << "track=" << Info.Track << "\n"
               << "last_frame=" << Info.Frame << "\n"
               << "last_pts=" << Info.PTS << "\n"
               << "last_timestamp_ms=" << Info.TimestampMs << "\n"
               << "last_file_position=" << Info.FilePosition << "\n"
               << "no_progress_ms=" << Info.NoProgressMs << "\n"
               << "detected_unix_ms=" << UnixNowMs() << "\n";
        std::string Ignored;
        WriteTextFileAtomic(ReportPath, Report.str(), &Ignored);
    }

    AbortedFlag.store(true, std::memory_order_release);
    if (Options.StallCallback)
        Options.StallCallback(&Info, Options.StallPrivate);
}

int StallMonitor::InterruptCallback(void *Opaque) {
    StallMonitor *Monitor = static_cast<StallMonitor *>(Opaque);
    return Monitor && Monitor->AbortedFlag.load(std::memory_order_acquire) ? 1 : 0;
}

} // namespace robust
