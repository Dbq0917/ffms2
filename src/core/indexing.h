//  Copyright (c) 2007-2017 Fredrik Mellbin
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

#ifndef INDEXING_H
#define INDEXING_H

#include "utils.h"
#include "robust.h"

#include <set>
#include <map>
#include <memory>
#include <atomic>

extern "C" {
#include <libavutil/avutil.h>
}

class Wave64Writer;
class ZipFile;

struct SharedAVContext {
    AVCodecContext *CodecContext = nullptr;
    AVCodecParserContext *Parser = nullptr;
    int64_t CurrentSample = 0;
    ~SharedAVContext();
};

struct FFMS_Index : public std::vector<FFMS_Track> {
    FFMS_Index(FFMS_Index const&) = delete;
    FFMS_Index& operator=(FFMS_Index const&) = delete;
    void ReadIndex(ZipFile &zf, const char* IndexFile);
    void WriteIndex(ZipFile &zf);
public:
    static void CalculateFileSignature(const char *Filename, int64_t *Filesize, uint8_t Digest[20],
        robust::StallMonitor *Monitor = nullptr);

    int ErrorHandling;
    int64_t Filesize;
    uint8_t Digest[20];
    std::map<std::string, std::string> LAVFOpts;

    void Finalize(std::vector<SharedAVContext> const& video_contexts, const char *Format);
    bool CompareFileSignature(const char *Filename, robust::StallMonitor *Monitor = nullptr);
    void WriteIndexFile(const char *IndexFile);
    uint8_t *WriteIndexBuffer(size_t *Size);

    FFMS_Index(const char *IndexFile);
    FFMS_Index(const uint8_t *Buffer, size_t Size);
    FFMS_Index(int64_t Filesize, uint8_t Digest[20], int ErrorHandling, const std::map<std::string, std::string> &LAVFOpts);
};

struct FFMS_Indexer {
private:
    std::map<int, FFMS_AudioProperties> LastAudioProperties;
    FFMS_Indexer(FFMS_Indexer const&) = delete;
    FFMS_Indexer& operator=(FFMS_Indexer const&) = delete;
    AVFormatContext *FormatContext = nullptr;
    std::set<int> IndexMask;
    std::map<std::string, std::string> LAVFOpts;
    int ErrorHandling = FFMS_IEH_CLEAR_TRACK;
    TIndexCallback IC = nullptr;
    void *ICPrivate = nullptr;
    std::string SourceFile;
    AVFrame *DecodeFrame = nullptr;
    robust::Settings RobustOptions;
    std::unique_ptr<robust::StallMonitor> Monitor;
    int64_t LastCheckpointWriteNs = 0;
    int64_t LastFrame = -1;
    int64_t LastPTS = AV_NOPTS_VALUE;
    int64_t LastTimestampMs = -1;
    int64_t LastFilePosition = -1;
    int LastVideoTrack = -1;
    int64_t LastPacketPos = -1;
    int64_t LastPacketPTS = AV_NOPTS_VALUE;
    int64_t LastPacketDTS = AV_NOPTS_VALUE;
    int LastPacketStream = -1;
    int LastPacketFlags = 0;
    int64_t ResumedFromFrame = -1;

    int64_t Filesize;
    uint8_t Digest[20];

    void ReadTS(const AVPacket &Packet, int64_t &TS, bool &UseDTS);
    void CheckAudioProperties(int Track, AVCodecContext *Context);
    uint32_t IndexAudioPacket(int Track, const AVPacket &Packet, SharedAVContext &Context, FFMS_Index &TrackIndices);
    void ParseVideoPacket(SharedAVContext &VideoContext, const AVPacket &pkt, int *RepeatPict, int *FrameType, bool *Invisible, bool *SecondField, enum AVPictureStructure *LastPicStruct);
    void Free();
    int TryRestoreCheckpoint(FFMS_Index &TrackIndices, std::vector<SharedAVContext> &AVContexts,
        std::vector<int64_t> &LastValidTS, enum AVPictureStructure &LastPicStruct);
    void WriteCheckpoint(const FFMS_Index &TrackIndices, int Status, const std::string &Reason,
        bool Finalized, const std::vector<SharedAVContext> &AVContexts,
        const std::vector<int64_t> &LastValidTS,
        enum AVPictureStructure LastPicStruct);
public:
    FFMS_Indexer(const char *Filename, const FFMS_KeyValuePair *DemuxerOptions, int NumOptions);
    FFMS_Indexer(const char *Filename, const FFMS_KeyValuePair *DemuxerOptions, int NumOptions,
        const FFMS_RobustOptions *Options);
    ~FFMS_Indexer();

    void SetIndexTrack(int Track, bool Index);
    void SetIndexTrackType(int TrackType, bool Index);
    void SetErrorHandling(int ErrorHandling_);
    void SetProgressCallback(TIndexCallback IC_, void *ICPrivate_);

    FFMS_Index *DoIndexing();
    int GetNumberOfTracks();
    FFMS_TrackType GetTrackType(int Track);
    const char *GetTrackCodec(int Track);
    const char *GetTrackMetadata(int Track, const char *Key);
    const char *GetFormatName();
};


#endif
