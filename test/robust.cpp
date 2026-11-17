#include <ffms.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int FFMS_CC CancelAfter40Percent(int64_t Current, int64_t Total, void *Private) {
    bool *Cancelled = static_cast<bool *>(Private);
    if (!*Cancelled && Total > 0 && Current * 100 / Total >= 40) {
        *Cancelled = true;
        return 1;
    }
    return 0;
}

bool ReadInfo(const char *Checkpoint, FFMS_CheckpointInfo *Info) {
    char ErrorBuffer[1024] = {};
    FFMS_ErrorInfo Error = {};
    Error.Buffer = ErrorBuffer;
    Error.BufferSize = sizeof(ErrorBuffer);
    if (FFMS_ReadCheckpoint(Checkpoint, Info, &Error) != FFMS_ERROR_SUCCESS) {
        std::fprintf(stderr, "ReadCheckpoint failed: %s\n", ErrorBuffer);
        return false;
    }
    return true;
}

int FFMS_CC RecordFirstProgress(int64_t Current, int64_t Total, void *Private) {
    int64_t *First = static_cast<int64_t *>(Private);
    if (*First < 0)
        *First = Total > 0 ? Current * 100 / Total : Current;
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s file\n", argv[0]);
        return 2;
    }

    const char *Source = argv[1];
    const std::string Checkpoint = std::string(Source) + ".ffms2-index.checkpoint.txt";
    FFMS_Init(0, 0);

    FFMS_RobustOptions Options = {};
    Options.EnableStallDetection = 1;
    Options.StallTimeoutMs = 5000;
    Options.EnableCheckpoint = 1;
    Options.CheckpointIntervalMs = 5000;

    char ErrorBuffer[1024] = {};
    FFMS_ErrorInfo Error = {};
    Error.Buffer = ErrorBuffer;
    Error.BufferSize = sizeof(ErrorBuffer);

    FFMS_Indexer *Indexer = FFMS_CreateIndexerWithOptions(Source, nullptr, 0, &Options, &Error);
    if (!Indexer) {
        std::fprintf(stderr, "CreateIndexer failed: %s\n", ErrorBuffer);
        return 1;
    }

    FFMS_TrackTypeIndexSettings(Indexer, FFMS_TYPE_AUDIO, 1, 0);
    bool Cancelled = false;
    FFMS_SetProgressCallback(Indexer, CancelAfter40Percent, &Cancelled);
    FFMS_DestroyIndex(FFMS_DoIndexing2(Indexer, FFMS_IEH_CLEAR_TRACK, &Error));
    if (!Cancelled) {
        std::fprintf(stderr, "test did not cancel the first indexing pass\n");
        return 1;
    }

    FFMS_CheckpointInfo Info = {};
    if (!ReadInfo(Checkpoint.c_str(), &Info))
        return 1;
    if (Info.Status != FFMS_CHECKPOINT_CANCELLED || Info.LastFrame < 0) {
        std::fprintf(stderr, "unexpected checkpoint after cancel: status=%d frame=%lld\n",
            Info.Status, static_cast<long long>(Info.LastFrame));
        return 1;
    }

    Indexer = FFMS_CreateIndexerWithOptions(Source, nullptr, 0, &Options, &Error);
    if (!Indexer) {
        std::fprintf(stderr, "CreateIndexer (resume) failed: %s\n", ErrorBuffer);
        return 1;
    }
    FFMS_TrackTypeIndexSettings(Indexer, FFMS_TYPE_AUDIO, 1, 0);
    int64_t FirstResumePercent = -1;
    FFMS_SetProgressCallback(Indexer, RecordFirstProgress, &FirstResumePercent);
    FFMS_Index *Index = FFMS_DoIndexing2(Indexer, FFMS_IEH_CLEAR_TRACK, &Error);
    if (!Index) {
        std::fprintf(stderr, "Resumed indexing failed: %s\n", ErrorBuffer);
        return 1;
    }

    if (!ReadInfo(Checkpoint.c_str(), &Info) || Info.Status != FFMS_CHECKPOINT_COMPLETED) {
        std::fprintf(stderr, "checkpoint was not marked completed\n");
        FFMS_DestroyIndex(Index);
        return 1;
    }

    int VideoTrack = FFMS_GetFirstIndexedTrackOfType(Index, FFMS_TYPE_VIDEO, &Error);
    if (VideoTrack < 0) {
        std::fprintf(stderr, "no video track: %s\n", ErrorBuffer);
        FFMS_DestroyIndex(Index);
        return 1;
    }

    FFMS_Track *Track = FFMS_GetTrackFromIndex(Index, VideoTrack);
    int Frames = FFMS_GetNumFrames(Track);
    FFMS_VideoSource *Video = FFMS_CreateVideoSourceWithOptions(Source, VideoTrack, Index,
        1, FFMS_SEEK_NORMAL, &Options, &Error);
    if (!Video) {
        std::fprintf(stderr, "CreateVideoSource failed: %s\n", ErrorBuffer);
        FFMS_DestroyIndex(Index);
        return 1;
    }

    for (int Frame = 0; Frame < Frames; ++Frame) {
        if (!FFMS_GetFrame(Video, Frame, &Error)) {
            std::fprintf(stderr, "GetFrame(%d) failed: %s\n", Frame, ErrorBuffer);
            FFMS_DestroyVideoSource(Video);
            FFMS_DestroyIndex(Index);
            return 1;
        }
    }

    const FFMS_VideoProperties *Properties = FFMS_GetVideoProperties(Video);
    if (Properties->NumFrames != Frames || Properties->LastEndTime <= 0) {
        std::fprintf(stderr, "unexpected output properties\n");
        FFMS_DestroyVideoSource(Video);
        FFMS_DestroyIndex(Index);
        return 1;
    }

    FFMS_Indexer *LegacyIndexer = FFMS_CreateIndexer(Source, &Error);
    if (!LegacyIndexer) {
        std::fprintf(stderr, "Legacy CreateIndexer failed: %s\n", ErrorBuffer);
        FFMS_DestroyVideoSource(Video);
        FFMS_DestroyIndex(Index);
        return 1;
    }
    FFMS_TrackTypeIndexSettings(LegacyIndexer, FFMS_TYPE_AUDIO, 1, 0);
    FFMS_Index *LegacyIndex = FFMS_DoIndexing2(LegacyIndexer, FFMS_IEH_CLEAR_TRACK, &Error);
    if (!LegacyIndex) {
        std::fprintf(stderr, "Legacy indexing failed: %s\n", ErrorBuffer);
        FFMS_DestroyVideoSource(Video);
        FFMS_DestroyIndex(Index);
        return 1;
    }
    if (FFMS_GetNumTracks(Index) != FFMS_GetNumTracks(LegacyIndex)) {
        std::fprintf(stderr, "track count differs from a clean index\n");
        FFMS_DestroyIndex(LegacyIndex);
        FFMS_DestroyVideoSource(Video);
        FFMS_DestroyIndex(Index);
        return 1;
    }
    for (int TrackNumber = 0; TrackNumber < FFMS_GetNumTracks(Index); ++TrackNumber) {
        FFMS_Track *ResumedTrack = FFMS_GetTrackFromIndex(Index, TrackNumber);
        FFMS_Track *CleanTrack = FFMS_GetTrackFromIndex(LegacyIndex, TrackNumber);
        if (FFMS_GetNumFrames(ResumedTrack) != FFMS_GetNumFrames(CleanTrack) ||
            FFMS_GetTrackType(ResumedTrack) != FFMS_GetTrackType(CleanTrack)) {
            std::fprintf(stderr, "track %d differs from a clean index\n", TrackNumber);
            FFMS_DestroyIndex(LegacyIndex);
            FFMS_DestroyVideoSource(Video);
            FFMS_DestroyIndex(Index);
            return 1;
        }
        int Count = FFMS_GetNumFrames(CleanTrack);
        if (FFMS_GetTrackType(CleanTrack) == FFMS_TYPE_VIDEO && Count > 0 &&
            FFMS_GetFrameInfo(CleanTrack, Count - 1)->PTS !=
                FFMS_GetFrameInfo(ResumedTrack, Count - 1)->PTS) {
            std::fprintf(stderr, "track %d last PTS differs from a clean index\n", TrackNumber);
            FFMS_DestroyIndex(LegacyIndex);
            FFMS_DestroyVideoSource(Video);
            FFMS_DestroyIndex(Index);
            return 1;
        }
    }
    FFMS_DestroyIndex(LegacyIndex);

    std::printf("ok frames=%d last_end=%.3f checkpoint_status=%d resume_progress_percent=%lld\n",
        Frames, Properties->LastEndTime, Info.Status, static_cast<long long>(FirstResumePercent));
    FFMS_DestroyVideoSource(Video);
    FFMS_DestroyIndex(Index);
    return 0;
}
