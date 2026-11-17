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

#include "indexing.h"

#include "track.h"
#include "videoutils.h"
#include "zipfile.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/sha.h>
}

#define INDEXID 0x53920873
#define INDEX_VERSION 8

namespace {

std::string HexDigest(const uint8_t Digest[20]) {
    char Buffer[41];
    for (int i = 0; i < 20; ++i)
        std::snprintf(Buffer + i * 2, 3, "%02x", Digest[i]);
    Buffer[40] = 0;
    return Buffer;
}

std::string EscapeCheckpoint(const std::string &Value) {
    std::string Out;
    Out.reserve(Value.size());
    for (size_t i = 0; i < Value.size(); ++i) {
        switch (Value[i]) {
        case '\\': Out += "\\\\"; break;
        case '\n': Out += "\\n"; break;
        case '\r': Out += "\\r"; break;
        case '\t': Out += "\\t"; break;
        default: Out += Value[i]; break;
        }
    }
    return Out;
}

std::string UnescapeCheckpoint(const std::string &Value) {
    std::string Out;
    Out.reserve(Value.size());
    for (size_t i = 0; i < Value.size(); ++i) {
        if (Value[i] == '\\' && i + 1 < Value.size()) {
            char Next = Value[++i];
            if (Next == 'n')
                Out += '\n';
            else if (Next == 'r')
                Out += '\r';
            else if (Next == 't')
                Out += '\t';
            else
                Out += Next;
        } else {
            Out += Value[i];
        }
    }
    return Out;
}

std::string SerializeIndexMask(const std::set<int> &Mask) {
    std::ostringstream Out;
    for (std::set<int>::const_iterator It = Mask.begin(); It != Mask.end(); ++It) {
        if (It != Mask.begin())
            Out << ',';
        Out << *It;
    }
    return Out.str();
}

std::map<std::string, std::string> SplitCheckpointSections(const std::string &Body) {
    std::map<std::string, std::string> Sections;
    std::istringstream In(Body);
    std::string Line;
    std::string Current;
    while (std::getline(In, Line)) {
        if (!Line.empty() && Line[Line.size() - 1] == '\r')
            Line.pop_back();
        if (!Line.empty() && Line[0] == '[' && Line[Line.size() - 1] == ']') {
            Current = Line.substr(1, Line.size() - 2);
            Sections[Current] = "";
        } else if (!Current.empty()) {
            Sections[Current] += Line + "\n";
        }
    }
    return Sections;
}

std::vector<std::string> SplitTab(const std::string &Value) {
    std::vector<std::string> Parts;
    size_t Begin = 0;
    while (true) {
        size_t Tab = Value.find('\t', Begin);
        if (Tab == std::string::npos) {
            Parts.push_back(Value.substr(Begin));
            break;
        }
        Parts.push_back(Value.substr(Begin, Tab - Begin));
        Begin = Tab + 1;
    }
    return Parts;
}

std::map<std::string, std::string> ParseKeyPairs(const std::string &Section) {
    std::map<std::string, std::string> Values;
    std::istringstream In(Section);
    std::string Line;
    while (std::getline(In, Line)) {
        size_t Eq = Line.find('=');
        if (Eq != std::string::npos)
            Values[Line.substr(0, Eq)] = UnescapeCheckpoint(Line.substr(Eq + 1));
    }
    return Values;
}

int RejectCheckpoint(const char *Reason) {
    if (std::getenv("FFMS_DEBUG_CHECKPOINT"))
        std::fprintf(stderr, "FFMS checkpoint resume rejected: %s\n", Reason);
    return 0;
}

} // namespace

SharedAVContext::~SharedAVContext() {
    avcodec_free_context(&CodecContext);
    if (Parser)
        av_parser_close(Parser);
}

void FFMS_Index::CalculateFileSignature(const char *Filename, int64_t *Filesize, uint8_t Digest[20],
    robust::StallMonitor *Monitor) {
    std::unique_ptr<AVSHA, decltype(&av_free)> ctx{ av_sha_alloc(), av_free };
    av_sha_init(ctx.get(), 160);

    try {
        FileHandle file(Filename, "rb", FFMS_ERROR_INDEX, FFMS_ERROR_FILE_READ,
            Monitor ? Monitor->GetInterruptCallback() : nullptr);
        *Filesize = file.Size();
        if (Monitor)
            Monitor->Touch();
        std::vector<char> FileBuffer(static_cast<size_t>(std::min<int64_t>(1024 * 1024, *Filesize)));
        size_t BytesRead = file.Read(FileBuffer.data(), FileBuffer.size());
        if (Monitor) {
            Monitor->Touch();
            Monitor->Check("Reading the source file signature");
        }
        av_sha_update(ctx.get(), reinterpret_cast<const uint8_t*>(FileBuffer.data()), BytesRead);

        if (*Filesize > static_cast<int64_t>(FileBuffer.size())) {
            file.Seek(*Filesize - static_cast<int64_t>(FileBuffer.size()), SEEK_SET);
            if (Monitor)
                Monitor->Touch();
            BytesRead = file.Read(FileBuffer.data(), FileBuffer.size());
            if (Monitor) {
                Monitor->Touch();
                Monitor->Check("Reading the source file signature");
            }
            av_sha_update(ctx.get(), reinterpret_cast<const uint8_t*>(FileBuffer.data()), BytesRead);
        }
    } catch (...) {
        if (Monitor)
            Monitor->Check("Reading the source file signature");
        av_sha_final(ctx.get(), Digest);
        throw;
    }
    av_sha_final(ctx.get(), Digest);
}

void FFMS_Index::Finalize(std::vector<SharedAVContext> const& video_contexts, const char *Format) {
    for (size_t i = 0, end = size(); i != end; ++i) {
        FFMS_Track& track = (*this)[i];

        if (!strcmp(Format, "mpeg") || !strcmp(Format, "mpegts") || !strcmp(Format, "mpegtsraw"))
            if (std::any_of(track.begin(), track.end(), [](FrameInfo F) { return F.PTS == AV_NOPTS_VALUE; }))
                track.RevertToDTS();

        // Some audio tracks are simply insane junk (seen with als) and will have a single(?) super long packet and
        // apart from that look legit and be chosen instead of usable audio. This hopefully rejects some of it.
        // Caused by sample in https://github.com/FFMS/ffms2/issues/351
        if (track.TT == FFMS_TYPE_AUDIO) {
            if (track.size() <= 10 && track.size() > 0 && track[0].SampleCount > 1000000)
                track.clear();
        }

        // H.264 PAFF needs to have some frames hidden
        //
        // Don't send any WMV/ASF files, since they (as far as we know) cannot contain PAFF,
        // but may also have valid, split packets, with pos equal to the previous pos.
        if (video_contexts[i].CodecContext && video_contexts[i].CodecContext->codec_id == AV_CODEC_ID_H264 && !!strcmp(Format, "asf"))
            track.MaybeHideFrames();

        track.FinalizeTrack();

        if (track.TT != FFMS_TYPE_VIDEO) continue;

        if (video_contexts[i].CodecContext && video_contexts[i].CodecContext->has_b_frames) {
            track.MaxBFrames = video_contexts[i].CodecContext->has_b_frames;
            continue;
        }

        // Whether or not has_b_frames gets set during indexing seems
        // to vary based on version of FFmpeg/Libav, so do an extra
        // check for b-frames if it's 0.
        for (size_t f = 0; f < track.size(); ++f) {
            if (track[f].FrameType == AV_PICTURE_TYPE_B) {
                track.MaxBFrames = 1;
                break;
            }
        }
    }
}

bool FFMS_Index::CompareFileSignature(const char *Filename, robust::StallMonitor *Monitor) {
    int64_t CFilesize;
    uint8_t CDigest[20];
    CalculateFileSignature(Filename, &CFilesize, CDigest, Monitor);
    if (Monitor)
        Monitor->Check("Reading the source file signature");
    return (CFilesize == Filesize && !memcmp(CDigest, Digest, sizeof(Digest)));
}

void FFMS_Index::WriteIndex(ZipFile &zf) {
    // Write the index file header
    zf.Write<uint32_t>(INDEXID);
    zf.Write<uint32_t>(FFMS_VERSION);
    zf.Write<uint16_t>(INDEX_VERSION);
    zf.Write<uint32_t>(size());
    zf.Write<uint32_t>(ErrorHandling);
    zf.Write<uint32_t>(avutil_version());
    zf.Write<uint32_t>(avformat_version());
    zf.Write<uint32_t>(avcodec_version());
    zf.Write<uint32_t>(swscale_version());
    zf.Write<int64_t>(Filesize);
    zf.Write(Digest);

    zf.Write<uint32_t>(LAVFOpts.size());
    for (const auto &iter : LAVFOpts) {
        zf.Write<uint32_t>(iter.first.length());
        zf.Write(iter.first.data(), iter.first.length());
        zf.Write<uint32_t>(iter.second.length());
        zf.Write(iter.second.data(), iter.second.length());
    }

    for (size_t i = 0; i < size(); ++i)
        at(i).Write(zf);

    zf.Finish();
}

void FFMS_Index::WriteIndexFile(const char *IndexFile) {
    ZipFile zf(IndexFile, "wb");

    WriteIndex(zf);
}

uint8_t *FFMS_Index::WriteIndexBuffer(size_t *Size) {
    ZipFile zf;

    WriteIndex(zf);

    return zf.GetBuffer(Size);
}

void FFMS_Index::ReadIndex(ZipFile &zf, const char *IndexFile) {
    // Read the index file header
    if (zf.Read<uint32_t>() != INDEXID)
        throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
            std::string("'") + IndexFile + "' is not a valid index file");

    if (zf.Read<uint32_t>() != FFMS_VERSION)
        throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
            std::string("'") + IndexFile + "' was not created with the expected FFMS2 version");

    if (zf.Read<uint16_t>() != INDEX_VERSION)
        throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
            std::string("'") + IndexFile + "' is not the expected index version");

    uint32_t Tracks = zf.Read<uint32_t>();
    ErrorHandling = zf.Read<uint32_t>();

    if (zf.Read<uint32_t>() != avutil_version() ||
        zf.Read<uint32_t>() != avformat_version() ||
        zf.Read<uint32_t>() != avcodec_version() ||
        zf.Read<uint32_t>() != swscale_version())
        throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
            std::string("A different FFmpeg build was used to create '") + IndexFile + "'");

    Filesize = zf.Read<int64_t>();
    zf.Read(Digest, sizeof(Digest));

    uint32_t NumOptions = zf.Read<uint32_t>();
    std::vector<char> KeyBuffer;
    std::vector<char> ValueBuffer;
    for (uint32_t i = 0; i < NumOptions; i++) {
        uint32_t KeyLength = zf.Read<uint32_t>();
        KeyBuffer.resize(KeyLength);
        zf.Read(KeyBuffer.data(), KeyLength);

        uint32_t ValueLength = zf.Read<uint32_t>();
        ValueBuffer.resize(ValueLength);
        zf.Read(ValueBuffer.data(), ValueLength);

        LAVFOpts[std::string(KeyBuffer.data(), KeyLength)] = std::string(ValueBuffer.data(), ValueLength);
    }

    reserve(Tracks);
    try {
        for (size_t i = 0; i < Tracks; ++i)
            emplace_back(zf);
    } catch (FFMS_Exception const&) {
        throw;
    } catch (...) {
        throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
            std::string("Unknown error while reading index information in '") + IndexFile + "'");
    }
}

FFMS_Index::FFMS_Index(const char *IndexFile) {
    ZipFile zf(IndexFile, "rb");

    ReadIndex(zf, IndexFile);
}

FFMS_Index::FFMS_Index(const uint8_t *Buffer, size_t Size) {
    ZipFile zf(Buffer, Size);

    ReadIndex(zf, "User supplied buffer");
}

FFMS_Index::FFMS_Index(int64_t Filesize, uint8_t Digest[20], int ErrorHandling, const std::map<std::string, std::string> &LAVFOpts)
    : ErrorHandling(ErrorHandling)
    , Filesize(Filesize), LAVFOpts(LAVFOpts) {
    memcpy(this->Digest, Digest, sizeof(this->Digest));
}

void FFMS_Indexer::SetIndexTrack(int Track, bool Index) {
    if (Track < 0 || Track >= GetNumberOfTracks())
        return;
    if (Index)
        IndexMask.insert(Track);
    else
        IndexMask.erase(Track);
};

void FFMS_Indexer::SetIndexTrackType(int TrackType, bool Index) {
    for (int i = 0; i < GetNumberOfTracks(); i++) {
        if (GetTrackType(i) == TrackType) {
            if (Index)
                IndexMask.insert(i);
            else
                IndexMask.erase(i);
        }
    }
}

void FFMS_Indexer::SetErrorHandling(int ErrorHandling_) {
    if (ErrorHandling_ != FFMS_IEH_ABORT && ErrorHandling_ != FFMS_IEH_CLEAR_TRACK &&
        ErrorHandling_ != FFMS_IEH_STOP_TRACK && ErrorHandling_ != FFMS_IEH_IGNORE)
        throw FFMS_Exception(FFMS_ERROR_INDEXING, FFMS_ERROR_INVALID_ARGUMENT,
            "Invalid error handling mode specified");
    ErrorHandling = ErrorHandling_;
}

void FFMS_Indexer::SetProgressCallback(TIndexCallback IC_, void *ICPrivate_) {
    IC = IC_;
    ICPrivate = ICPrivate_;
}

FFMS_Indexer::FFMS_Indexer(const char *Filename, const FFMS_KeyValuePair *DemuxerOptions, int NumOptions)
    : FFMS_Indexer(Filename, DemuxerOptions, NumOptions, nullptr) {
}

FFMS_Indexer::FFMS_Indexer(const char *Filename, const FFMS_KeyValuePair *DemuxerOptions, int NumOptions,
    const FFMS_RobustOptions *Options)
    : SourceFile(Filename)
    , RobustOptions(robust::Settings::FromOptions(Options, Filename, FFMS_CHECKPOINT_INDEX))
    , Monitor(new robust::StallMonitor(RobustOptions, Filename)) {
    try {
        Monitor->Start();
        Monitor->SetPhase(FFMS_ACTIVITY_OPENING, "avformat_open_input");

        AVDictionary *Dict = nullptr;
        for (int i = 0; i < NumOptions; i++) {
            if (!DemuxerOptions[i].Key && !DemuxerOptions[i].Value)
                throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_INVALID_ARGUMENT,
                    "Demuxer key-value pairs can't have NULL strings");
            LAVFOpts[DemuxerOptions[i].Key] = DemuxerOptions[i].Value;
            av_dict_set(&Dict, DemuxerOptions[i].Key, DemuxerOptions[i].Value, 0);
        }

        FormatContext = avformat_alloc_context();
        if (!FormatContext) {
            av_dict_free(&Dict);
            throw FFMS_Exception(FFMS_ERROR_INDEXING, FFMS_ERROR_ALLOCATION_FAILED,
                "Couldn't allocate format context");
        }
        Monitor->Install(FormatContext);

        if (avformat_open_input(&FormatContext, Filename, nullptr, &Dict) != 0) {
            Monitor->Check("Opening input");
            av_dict_free(&Dict);
            throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
                std::string("Can't open '") + Filename + "'");
        }

        av_dict_free(&Dict);

        Monitor->SetPhase(FFMS_ACTIVITY_OPENING, "reading the FFMS source signature (avio_read)");
        FFMS_Index::CalculateFileSignature(Filename, &Filesize, Digest, Monitor.get());

        Monitor->SetPhase(FFMS_ACTIVITY_OPENING, "avformat_find_stream_info");
        if (avformat_find_stream_info(FormatContext, nullptr) < 0) {
            Monitor->Check("Reading stream information");
            avformat_close_input(&FormatContext);
            throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
                "Couldn't find stream information");
        }
        Monitor->Check("Reading stream information");

        for (unsigned int i = 0; i < FormatContext->nb_streams; i++)
            if (FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                IndexMask.insert(i);

        DecodeFrame = av_frame_alloc();
        if (!DecodeFrame)
            throw FFMS_Exception(FFMS_ERROR_DECODING, FFMS_ERROR_ALLOCATION_FAILED,
                "Couldn't allocate frame");
    } catch (FFMS_Exception &e) {
        robust::WriteFailureReport(RobustOptions, SourceFile, "opening/index initialization",
            Monitor->Aborted() ? FFMS_CHECKPOINT_STALLED : FFMS_CHECKPOINT_FAILED,
            e.GetErrorMessage(), -1, -1, AV_NOPTS_VALUE, -1, -1);
        Free();
        throw;
    } catch (...) {
        Free();
        throw;
    }
}

uint32_t FFMS_Indexer::IndexAudioPacket(int Track, const AVPacket &Packet, SharedAVContext &Context, FFMS_Index &TrackIndices) {
    AVCodecContext *CodecContext = Context.CodecContext;
    int64_t StartSample = Context.CurrentSample;
    int Ret = avcodec_send_packet(CodecContext, &Packet);
    if (Ret != 0) {
        if (ErrorHandling == FFMS_IEH_ABORT) {
            throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_DECODING, "Audio decoding error");
        } else if (ErrorHandling == FFMS_IEH_CLEAR_TRACK) {
            TrackIndices[Track].clear();
            IndexMask.erase(Track);
        } else if (ErrorHandling == FFMS_IEH_STOP_TRACK) {
            IndexMask.erase(Track);
        }
    }

    while (true) {
        av_frame_unref(DecodeFrame);
        Ret = avcodec_receive_frame(CodecContext, DecodeFrame);
        if (Ret == 0) {
            CheckAudioProperties(Track, CodecContext);
            Context.CurrentSample += DecodeFrame->nb_samples;
        } else if (Ret == AVERROR_EOF || Ret == AVERROR(EAGAIN)) {
            break;
        } else {
            if (ErrorHandling == FFMS_IEH_ABORT) {
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_DECODING, "Audio decoding error");
            } else if (ErrorHandling == FFMS_IEH_CLEAR_TRACK) {
                TrackIndices[Track].clear();
                IndexMask.erase(Track);
            } else if (ErrorHandling == FFMS_IEH_STOP_TRACK) {
                IndexMask.erase(Track);
            }
        }
    }

    return static_cast<uint32_t>(Context.CurrentSample - StartSample);
}

void FFMS_Indexer::CheckAudioProperties(int Track, AVCodecContext *Context) {
    auto it = LastAudioProperties.find(Track);
    if (it == LastAudioProperties.end()) {
        FFMS_AudioProperties &AP = LastAudioProperties[Track];
        AP.SampleRate = Context->sample_rate;
        AP.SampleFormat = Context->sample_fmt;
        AP.Channels = Context->ch_layout.nb_channels;
    } else if (it->second.SampleRate != Context->sample_rate ||
        it->second.SampleFormat != Context->sample_fmt ||
        it->second.Channels != Context->ch_layout.nb_channels) {
        std::ostringstream buf; // fixme, dodgy comparison and maybe wrong since it skips layout comp
        buf <<
            "Audio format change detected. This is currently unsupported."
            << " Channels: " << it->second.Channels << " -> " << Context->ch_layout.nb_channels << ";"
            << " Sample rate: " << it->second.SampleRate << " -> " << Context->sample_rate << ";"
            << " Sample format: " << av_get_sample_fmt_name((AVSampleFormat)it->second.SampleFormat) << " -> "
            << av_get_sample_fmt_name(Context->sample_fmt);
        throw FFMS_Exception(FFMS_ERROR_UNSUPPORTED, FFMS_ERROR_DECODING, buf.str());
    }
}

void FFMS_Indexer::ParseVideoPacket(SharedAVContext &VideoContext, const AVPacket &pkt, int *RepeatPict,
                                    int *FrameType, bool *Invisible, bool *SecondField, enum AVPictureStructure *LastPicStruct) {
    if (VideoContext.Parser) {
        uint8_t *OB;
        int OBSize;

        av_parser_parse2(VideoContext.Parser,
            VideoContext.CodecContext,
            &OB, &OBSize,
            pkt.data, pkt.size,
            pkt.pts, pkt.dts, pkt.pos);

        // H.264 (PAFF) and HEVC may have one field per packet, so we need to track
        // when we have a full or half frame available, and mark one of them as
        // hidden, so we do not get duplicate frames.
        if (VideoContext.CodecContext->codec_id == AV_CODEC_ID_H264 ||
            VideoContext.CodecContext->codec_id == AV_CODEC_ID_HEVC) {
            if ((VideoContext.Parser->picture_structure == AV_PICTURE_STRUCTURE_TOP_FIELD &&
                 *LastPicStruct == AV_PICTURE_STRUCTURE_BOTTOM_FIELD) ||
                (VideoContext.Parser->picture_structure == AV_PICTURE_STRUCTURE_BOTTOM_FIELD &&
                 *LastPicStruct == AV_PICTURE_STRUCTURE_TOP_FIELD)) {
                *SecondField = true;
                *LastPicStruct = AV_PICTURE_STRUCTURE_UNKNOWN;
            } else {
                *LastPicStruct = VideoContext.Parser->picture_structure;
            }
        }

        *RepeatPict = VideoContext.Parser->repeat_pict;
        *FrameType = VideoContext.Parser->pict_type;
        *Invisible = (VideoContext.Parser->repeat_pict < 0 || (pkt.flags & AV_PKT_FLAG_DISCARD));
    } else {
        *Invisible = !!(pkt.flags & AV_PKT_FLAG_DISCARD);
    }

    if (VideoContext.CodecContext->codec_id == AV_CODEC_ID_VP8)
        ParseVP8(pkt.data[0], Invisible, FrameType);
    else if (VideoContext.CodecContext->codec_id == AV_CODEC_ID_VP9)
        ParseVP9(pkt.data[0], FrameType);
}

void FFMS_Indexer::Free() {
    if (Monitor)
        Monitor->Stop();
    av_frame_free(&DecodeFrame);
    avformat_close_input(&FormatContext);
}

FFMS_Indexer::~FFMS_Indexer() {
    Free();
}

void FFMS_Indexer::WriteCheckpoint(const FFMS_Index &TrackIndices, int Status,
    const std::string &Reason, bool Finalized,
    const std::vector<SharedAVContext> &AVContexts,
    const std::vector<int64_t> &LastValidTS,
    enum AVPictureStructure LastPicStruct) {
    if (!RobustOptions.Checkpoint)
        return;

    const int64_t Now = robust::SteadyNowNs();
    if (Status == FFMS_CHECKPOINT_RUNNING &&
        LastCheckpointWriteNs != 0 &&
        Now - LastCheckpointWriteNs < RobustOptions.CheckpointIntervalMs * 1000000)
        return;
    LastCheckpointWriteNs = Now;

    robust::CheckpointRecord Record;
    Record.Kind = FFMS_CHECKPOINT_INDEX;
    Record.Status = Status;
    Record.Finalized = Finalized;
    Record.Track = LastVideoTrack;
    Record.LastFrame = LastFrame;
    Record.LastPTS = LastPTS;
    Record.LastTimestampMs = LastTimestampMs;
    Record.LastFilePosition = LastFilePosition;
    Record.Source = SourceFile;
    Record.SourceSize = Filesize;
    Record.Reason = Reason;
    Record.UpdatedAtUnixMs = robust::UnixNowMs();
    Record.Extra["source_sha1"] = HexDigest(Digest);
    Record.Extra["ffms_version"] = std::to_string(FFMS_GetVersion());
    Record.Extra["index_format_version"] = std::to_string(INDEX_VERSION);
    Record.Extra["avutil_version"] = std::to_string(avutil_version());
    Record.Extra["avformat_version"] = std::to_string(avformat_version());
    Record.Extra["avcodec_version"] = std::to_string(avcodec_version());
    Record.Extra["swscale_version"] = std::to_string(swscale_version());
    Record.Extra["error_handling"] = std::to_string(ErrorHandling);
    Record.Extra["track_count"] = std::to_string(TrackIndices.size());
    Record.Extra["resumed"] = ResumedFromFrame >= 0 ? "1" : "0";
    Record.Extra["resumed_from_frame"] = std::to_string(ResumedFromFrame);

    std::ostringstream Body;
    Body << "[resume]\n"
         << "index_mask=" << EscapeCheckpoint(SerializeIndexMask(IndexMask)) << "\n"
         << "last_pic_struct=" << static_cast<int>(LastPicStruct) << "\n"
         << "last_packet_pos=" << LastPacketPos << "\n"
         << "last_packet_stream=" << LastPacketStream << "\n"
         << "last_packet_pts=" << LastPacketPTS << "\n"
         << "last_packet_dts=" << LastPacketDTS << "\n"
         << "last_packet_flags=" << LastPacketFlags << "\n"
         << "last_valid_ts=";
    for (size_t i = 0; i < LastValidTS.size(); ++i) {
        if (i)
            Body << ',';
        Body << LastValidTS[i];
    }
    Body << "\ncurrent_sample=";
    for (size_t i = 0; i < AVContexts.size(); ++i) {
        if (i)
            Body << ',';
        Body << AVContexts[i].CurrentSample;
    }
    Body << "\n\n[options]\n";
    for (std::map<std::string, std::string>::const_iterator It = LAVFOpts.begin(); It != LAVFOpts.end(); ++It)
        Body << "option=" << EscapeCheckpoint(It->first) << '\t' << EscapeCheckpoint(It->second) << "\n";

    for (size_t i = 0; i < TrackIndices.size(); ++i) {
        Body << "\n[track " << i << "]\n";
        TrackIndices[i].WriteCheckpoint(Body);
    }

    Record.Body = Body.str();
    std::string IgnoredError;
    robust::WriteCheckpoint(RobustOptions.CheckpointFile, Record, &IgnoredError);
}

int FFMS_Indexer::TryRestoreCheckpoint(FFMS_Index &TrackIndices,
    std::vector<SharedAVContext> &AVContexts, std::vector<int64_t> &LastValidTS,
    enum AVPictureStructure &LastPicStruct) {
    if (!RobustOptions.Checkpoint)
        return 0;

    robust::CheckpointRecord Record;
    std::string Error;
    if (!robust::ReadCheckpoint(RobustOptions.CheckpointFile, Record, &Error))
        return RejectCheckpoint(("cannot read checkpoint: " + Error).c_str());

    if (Record.Kind != FFMS_CHECKPOINT_INDEX ||
        Record.Source != SourceFile ||
        Record.SourceSize != Filesize)
        return RejectCheckpoint("checkpoint source or source size does not match");

    std::map<std::string, std::string>::const_iterator It;
    It = Record.Extra.find("source_sha1");
    if (It == Record.Extra.end() || It->second != HexDigest(Digest))
        return RejectCheckpoint("source digest changed");
    It = Record.Extra.find("ffms_version");
    if (It == Record.Extra.end() || std::strtoll(It->second.c_str(), nullptr, 10) != FFMS_GetVersion())
        return RejectCheckpoint("ffms2 version changed");
    It = Record.Extra.find("index_format_version");
    if (It == Record.Extra.end() || std::atoi(It->second.c_str()) != INDEX_VERSION)
        return RejectCheckpoint("index format version changed");
    It = Record.Extra.find("avutil_version");
    if (It == Record.Extra.end() || std::strtoull(It->second.c_str(), nullptr, 10) != avutil_version())
        return RejectCheckpoint("libavutil version changed");
    It = Record.Extra.find("avformat_version");
    if (It == Record.Extra.end() || std::strtoull(It->second.c_str(), nullptr, 10) != avformat_version())
        return RejectCheckpoint("libavformat version changed");
    It = Record.Extra.find("avcodec_version");
    if (It == Record.Extra.end() || std::strtoull(It->second.c_str(), nullptr, 10) != avcodec_version())
        return RejectCheckpoint("libavcodec version changed");
    It = Record.Extra.find("swscale_version");
    if (It == Record.Extra.end() || std::strtoull(It->second.c_str(), nullptr, 10) != swscale_version())
        return RejectCheckpoint("libswscale version changed");
    It = Record.Extra.find("error_handling");
    if (It == Record.Extra.end() || std::atoi(It->second.c_str()) != ErrorHandling)
        return RejectCheckpoint("error handling mode changed");
    It = Record.Extra.find("track_count");
    if (It == Record.Extra.end() || std::strtoull(It->second.c_str(), nullptr, 10) != TrackIndices.size())
        return RejectCheckpoint("track count changed");

    std::map<std::string, std::string> Sections = SplitCheckpointSections(Record.Body);
    std::map<std::string, std::string>::const_iterator ResumeIt = Sections.find("resume");
    std::map<std::string, std::string>::const_iterator OptionsIt = Sections.find("options");
    if (ResumeIt == Sections.end() || OptionsIt == Sections.end())
        return RejectCheckpoint("resume or options section is missing");

    std::map<std::string, std::string> Resume = ParseKeyPairs(ResumeIt->second);
    if (Resume["index_mask"] != SerializeIndexMask(IndexMask))
        return RejectCheckpoint("indexed track mask changed");

    std::map<std::string, std::string> SavedOptions;
    {
        std::istringstream In(OptionsIt->second);
        std::string Line;
        while (std::getline(In, Line)) {
            if (Line.compare(0, 7, "option=") != 0)
                continue;
            std::vector<std::string> Parts = SplitTab(UnescapeCheckpoint(Line.substr(7)));
            if (Parts.size() == 2)
                SavedOptions[Parts[0]] = Parts[1];
            else if (Parts.size() == 1)
                SavedOptions[Parts[0]] = "";
        }
    }
    if (SavedOptions != LAVFOpts)
        return RejectCheckpoint("demuxer options changed");

    std::vector<FFMS_Track> RestoredTracks(TrackIndices.size());
    for (size_t i = 0; i < TrackIndices.size(); ++i) {
        std::ostringstream Name;
        Name << "track " << i;
        std::map<std::string, std::string>::const_iterator Section = Sections.find(Name.str());
        if (Section == Sections.end())
            return RejectCheckpoint("track section is missing");
        if (!RestoredTracks[i].ReadCheckpoint(Section->second, Record.Finalized, Error))
            return RejectCheckpoint(("cannot restore track: " + Error).c_str());
        if (RestoredTracks[i].TT != TrackIndices[i].TT ||
            RestoredTracks[i].TB.Num != TrackIndices[i].TB.Num ||
            RestoredTracks[i].TB.Den != TrackIndices[i].TB.Den)
            return RejectCheckpoint("track type or time base changed");
    }

    LastFrame = Record.LastFrame;
    LastPTS = Record.LastPTS;
    LastTimestampMs = Record.LastTimestampMs;
    LastFilePosition = Record.LastFilePosition;
    LastVideoTrack = Record.Track;
    LastPacketPos = std::strtoll(Resume["last_packet_pos"].c_str(), nullptr, 10);
    LastPacketStream = std::atoi(Resume["last_packet_stream"].c_str());
    LastPacketPTS = std::strtoll(Resume["last_packet_pts"].c_str(), nullptr, 10);
    LastPacketDTS = std::strtoll(Resume["last_packet_dts"].c_str(), nullptr, 10);
    LastPacketFlags = std::atoi(Resume["last_packet_flags"].c_str());
    LastPicStruct = static_cast<enum AVPictureStructure>(std::atoi(Resume["last_pic_struct"].c_str()));

    if (Record.Status == FFMS_CHECKPOINT_COMPLETED && Record.Finalized) {
        ResumedFromFrame = Record.LastFrame;
        for (size_t i = 0; i < TrackIndices.size(); ++i)
            TrackIndices[i] = RestoredTracks[i];
        return 2;
    }

    if (LastPacketPos < 0)
        return RejectCheckpoint("checkpoint has no resumable packet position");

    bool Seeked = avformat_seek_file(FormatContext, -1, INT64_MIN, LastPacketPos, INT64_MAX,
        AVSEEK_FLAG_BYTE) >= 0;
    if (!Seeked && LastPacketStream >= 0) {
        int64_t TargetTS = LastPacketPTS != AV_NOPTS_VALUE ? LastPacketPTS : LastPacketDTS;
        if (TargetTS != AV_NOPTS_VALUE)
            Seeked = av_seek_frame(FormatContext, LastPacketStream, TargetTS,
                AVSEEK_FLAG_BACKWARD) >= 0;
    }
    if (!Seeked && LastVideoTrack >= 0 && LastPTS != AV_NOPTS_VALUE)
        Seeked = av_seek_frame(FormatContext, LastVideoTrack, LastPTS,
            AVSEEK_FLAG_BACKWARD) >= 0;
    if (!Seeked && FormatContext->pb)
        Seeked = avio_seek(FormatContext->pb, LastPacketPos, SEEK_SET) >= 0;
    if (!Seeked)
        return RejectCheckpoint("input cannot seek to the checkpoint position");
    avformat_flush(FormatContext);
    const auto ResumeFailed = [&]() {
        RejectCheckpoint("packet at the checkpoint position could not be matched");
        int RewindResult = avformat_seek_file(FormatContext, -1, INT64_MIN, 0, INT64_MAX,
            AVSEEK_FLAG_BYTE | AVSEEK_FLAG_BACKWARD);
        avformat_flush(FormatContext);
        if (RewindResult < 0)
            throw FFMS_Exception(FFMS_ERROR_CHECKPOINT, FFMS_ERROR_SEEKING,
                "Checkpoint resume failed and the input could not be rewound for a clean rebuild");
        return 0;
    };

    SmartAVPacket Packet;
    bool Matched = false;
    while (true) {
        int Ret = av_read_frame(FormatContext, Packet.get());
        if (Ret < 0)
            return ResumeFailed();
        if (Packet->pos > LastPacketPos) {
            av_packet_unref(Packet.get());
            return ResumeFailed();
        }
        if (Packet->pos == LastPacketPos &&
            Packet->stream_index == LastPacketStream &&
            Packet->pts == LastPacketPTS &&
            Packet->dts == LastPacketDTS &&
            Packet->flags == LastPacketFlags) {
            Matched = true;
            av_packet_unref(Packet.get());
            break;
        }
        av_packet_unref(Packet.get());
    }
    if (!Matched)
        return ResumeFailed();

    LastValidTS.assign(LastValidTS.size(), AV_NOPTS_VALUE);
    {
        std::istringstream In(Resume["last_valid_ts"]);
        std::string Value;
        size_t Index = 0;
        while (std::getline(In, Value, ',') && Index < LastValidTS.size())
            LastValidTS[Index++] = std::strtoll(Value.c_str(), nullptr, 10);
    }
    {
        std::istringstream In(Resume["current_sample"]);
        std::string Value;
        size_t Index = 0;
        while (std::getline(In, Value, ',') && Index < AVContexts.size())
            AVContexts[Index++].CurrentSample = std::strtoll(Value.c_str(), nullptr, 10);
    }

    for (size_t i = 0; i < TrackIndices.size(); ++i)
        TrackIndices[i] = RestoredTracks[i];
    ResumedFromFrame = Record.LastFrame;
    return 1;
}

int FFMS_Indexer::GetNumberOfTracks() {
    return FormatContext->nb_streams;
}

const char *FFMS_Indexer::GetFormatName() {
    return FormatContext->iformat->name;
}

FFMS_TrackType FFMS_Indexer::GetTrackType(int Track) {
    return static_cast<FFMS_TrackType>(FormatContext->streams[Track]->codecpar->codec_type);
}

const char *FFMS_Indexer::GetTrackCodec(int Track) {
    auto *codec = avcodec_find_decoder(FormatContext->streams[Track]->codecpar->codec_id);
    return codec ? codec->name : nullptr;
}

const char *FFMS_Indexer::GetTrackMetadata(int Track, const char *Key) {
    AVStream *stream = FormatContext->streams[Track];
    AVDictionaryEntry *Entry = av_dict_get(stream->metadata, Key, NULL, 0);
    return Entry ? Entry->value : nullptr;
}

FFMS_Index *FFMS_Indexer::DoIndexing() {
    std::vector<SharedAVContext> AVContexts(FormatContext->nb_streams);

    auto TrackIndices = std::unique_ptr<FFMS_Index>(new FFMS_Index(Filesize, Digest, ErrorHandling, LAVFOpts));
    bool UseDTS = !strcmp(FormatContext->iformat->name, "nuv");
    bool IsMpegLike = !strcmp(FormatContext->iformat->name, "mpeg") || !strcmp(FormatContext->iformat->name, "mpegts") || !strcmp(FormatContext->iformat->name, "mpegtsraw");

    for (unsigned int i = 0; i < FormatContext->nb_streams; i++) {
        TrackIndices->emplace_back((int64_t)FormatContext->streams[i]->time_base.num * 1000,
            FormatContext->streams[i]->time_base.den,
            static_cast<FFMS_TrackType>(FormatContext->streams[i]->codecpar->codec_type),
            !!(FormatContext->iformat->flags & AVFMT_TS_DISCONT),
            UseDTS);

        if (IndexMask.count(i) && FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            auto *VideoCodec = avcodec_find_decoder(FormatContext->streams[i]->codecpar->codec_id);
            if (!VideoCodec) {
                FormatContext->streams[i]->discard = AVDISCARD_ALL;
                IndexMask.erase(i);
                continue;
            }

            AVContexts[i].CodecContext = avcodec_alloc_context3(VideoCodec);
            if (AVContexts[i].CodecContext == nullptr)
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_ALLOCATION_FAILED,
                    "Could not allocate video codec context");

            if (avcodec_parameters_to_context(AVContexts[i].CodecContext, FormatContext->streams[i]->codecpar) < 0)
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_DECODING,
                    "Could not copy video codec parameters");

            if (avcodec_open2(AVContexts[i].CodecContext, VideoCodec, nullptr) < 0)
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_DECODING,
                    "Could not open video codec");

            AVContexts[i].Parser = av_parser_init(FormatContext->streams[i]->codecpar->codec_id);
            if (AVContexts[i].Parser)
                AVContexts[i].Parser->flags = PARSER_FLAG_COMPLETE_FRAMES;

            if (FormatContext->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                FormatContext->streams[i]->discard = AVDISCARD_ALL;
                IndexMask.erase(i);
            } else {
                IndexMask.insert(i);
            }
        } else if (IndexMask.count(i) && FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            auto *AudioCodec = avcodec_find_decoder(FormatContext->streams[i]->codecpar->codec_id);
            if (AudioCodec == nullptr)
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_UNSUPPORTED,
                    "Audio codec not found");

            AVContexts[i].CodecContext = avcodec_alloc_context3(AudioCodec);
            if (AVContexts[i].CodecContext == nullptr)
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_ALLOCATION_FAILED,
                    "Could not allocate audio codec context");

            if (avcodec_parameters_to_context(AVContexts[i].CodecContext, FormatContext->streams[i]->codecpar) < 0)
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_DECODING,
                    "Could not copy audio codec parameters");

            if (avcodec_open2(AVContexts[i].CodecContext, AudioCodec, nullptr) < 0)
                throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_DECODING,
                    "Could not open audio codec");

            (*TrackIndices)[i].HasTS = false;
        } else {
            FormatContext->streams[i]->discard = AVDISCARD_ALL;
            IndexMask.erase(i);
        }
    }

    SmartAVPacket Packet;
    std::vector<int64_t> LastValidTS(FormatContext->nb_streams, AV_NOPTS_VALUE);
    enum AVPictureStructure LastPicStruct = AV_PICTURE_STRUCTURE_UNKNOWN;

    const int RestoreResult = TryRestoreCheckpoint(*TrackIndices, AVContexts, LastValidTS, LastPicStruct);
    if (RestoreResult == 2) {
        Monitor->Stop();
        return TrackIndices.release();
    }

    std::vector<int64_t> VisibleFrames(FormatContext->nb_streams, 0);
    if (RestoreResult == 1) {
        for (size_t i = 0; i < TrackIndices->size(); ++i)
            for (size_t f = 0; f < (*TrackIndices)[i].size(); ++f)
                if (!(*TrackIndices)[i][f].Skipped())
                    VisibleFrames[i]++;
    } else {
        LastFrame = -1;
        LastPTS = AV_NOPTS_VALUE;
        LastTimestampMs = -1;
        LastFilePosition = -1;
        LastVideoTrack = -1;
        LastPacketPos = -1;
        LastPacketPTS = AV_NOPTS_VALUE;
        LastPacketDTS = AV_NOPTS_VALUE;
        LastPacketStream = -1;
        LastPacketFlags = 0;
    }

    int64_t filesize = avio_size(FormatContext->pb);
    int ret = 0;
    Monitor->SetPhase(FFMS_ACTIVITY_READING, "av_read_frame while indexing packets");
    Monitor->Touch();

    try {
        while ((ret = av_read_frame(FormatContext, Packet.get())) >= 0) {
            Monitor->Check("Reading packet");
            Monitor->Touch();

            // Update progress
            // FormatContext->pb can apparently be NULL when opening images.
            if (IC && FormatContext->pb) {
                if ((*IC)(FormatContext->pb->pos, filesize, ICPrivate)) {
                    throw FFMS_Exception(FFMS_ERROR_CANCELLED, FFMS_ERROR_USER,
                        "Cancelled by user");
                }
            }
            if (!IndexMask.count(Packet->stream_index)) {
                av_packet_unref(Packet.get());
                continue;
            }

            int Track = Packet->stream_index;
            FFMS_Track &TrackInfo = (*TrackIndices)[Track];
            bool KeyFrame = !!(Packet->flags & AV_PKT_FLAG_KEY);
            ReadTS(*Packet, LastValidTS[Track], (*TrackIndices)[Track].UseDTS);

            if (FormatContext->streams[Track]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                int64_t PTS = TrackInfo.UseDTS ? Packet->dts : Packet->pts;
                if (PTS == AV_NOPTS_VALUE && !IsMpegLike) {
                    // VPx alt-refs are output as packets which lack timestmps or durations, since
                    // they are invisible. Currently, the timestamp mangling code in libavformat
                    // will sometimes add a bogus timestamp and duration, if the webm in question
                    // has the block duration set (which is a hack, and not really a frame duration
                    // at all). In the future, libav* will only output packets without timestamps
                    // or duration, so we need to handle it here, regardless. This does not handle
                    // NVOPs. We set the duration based on the last PTS, for these packets, because
                    // FFMS2 currently sorts packets by PTS, which will break decoding, otherwise.
                    bool HasAltRefs = (FormatContext->streams[Track]->codecpar->codec_id == AV_CODEC_ID_VP8 ||
                                       FormatContext->streams[Track]->codecpar->codec_id == AV_CODEC_ID_VP9);
                    if (Packet->duration == 0 && !HasAltRefs) {
                        throw FFMS_Exception(FFMS_ERROR_INDEXING, FFMS_ERROR_PARSER,
                            "Invalid packet pts, dts, and duration");
                    }

                    if (TrackInfo.empty())
                        PTS = 0;
                    else
                        PTS = TrackInfo.back().PTS + TrackInfo.LastDuration;

                    TrackInfo.HasTS = false;
                }

                int RepeatPict = -1;
                int FrameType = 0;
                bool Invisible = false;
                bool SecondField = false;
                ParseVideoPacket(AVContexts[Track], *Packet, &RepeatPict, &FrameType, &Invisible, &SecondField, &LastPicStruct);

                TrackInfo.AddVideoFrame(PTS, Packet->dts, RepeatPict, KeyFrame,
                    FrameType, Packet->pos, Invisible, SecondField);

                if (!Invisible && !SecondField) {
                    LastFrame = VisibleFrames[Track]++;
                    LastPTS = PTS;
                    LastTimestampMs = av_rescale(PTS, TrackInfo.TB.Num, TrackInfo.TB.Den);
                    LastFilePosition = Packet->pos;
                    LastVideoTrack = Track;
                    Monitor->SetPosition(Track, LastFrame, LastPTS, LastTimestampMs, LastFilePosition);
                }
            } else if (FormatContext->streams[Track]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                // For video seeking timestamps are used only if all packets have
                // timestamps, while for audio they're used if any have timestamps,
                // as it's pretty common for only some packets to have timestamps
                if (LastValidTS[Track] != AV_NOPTS_VALUE)
                    TrackInfo.HasTS = true;

                int64_t StartSample = AVContexts[Track].CurrentSample;
                uint32_t SampleCount = IndexAudioPacket(Track, *Packet, AVContexts[Track], *TrackIndices);
                TrackInfo.SampleRate = AVContexts[Track].CodecContext->sample_rate;

                TrackInfo.AddAudioFrame(LastValidTS[Track], Packet->dts,
                    StartSample, SampleCount, KeyFrame, Packet->pos, Packet->flags & AV_PKT_FLAG_DISCARD);
            }

            if (!(Packet->flags & AV_PKT_FLAG_DISCARD))
                TrackInfo.LastDuration = Packet->duration;

            LastPacketPos = Packet->pos;
            LastPacketPTS = Packet->pts;
            LastPacketDTS = Packet->dts;
            LastPacketStream = Packet->stream_index;
            LastPacketFlags = Packet->flags;

            av_packet_unref(Packet.get());
            WriteCheckpoint(*TrackIndices, FFMS_CHECKPOINT_RUNNING, "", false,
                AVContexts, LastValidTS, LastPicStruct);
        }

        Monitor->Check("Reading input");
        if (ret == AVERROR_EOF && FormatContext->pb &&
            FormatContext->pb->error < 0 && FormatContext->pb->error != AVERROR_EOF)
            throw FFMS_Exception(FFMS_ERROR_INDEXING, FFMS_ERROR_FILE_READ,
                "Input ended unexpectedly at frame " + std::to_string(LastFrame) +
                " (timestamp " + std::to_string(LastTimestampMs) + " ms): " +
                AVErrorToString(FormatContext->pb->error));
        if (IsIOError(ret))
            throw FFMS_Exception(FFMS_ERROR_INDEXING, FFMS_ERROR_FILE_READ,
                "Indexing failed at frame " + std::to_string(LastFrame) +
                " (timestamp " + std::to_string(LastTimestampMs) + " ms): " + AVErrorToString(ret));

        Monitor->SetPhase(FFMS_ACTIVITY_FINALIZING, "FFMS_Index::Finalize");
        Monitor->Touch();
        TrackIndices->Finalize(AVContexts, FormatContext->iformat->name);
        Monitor->Check("Finalizing the index");

        if (LastVideoTrack >= 0 && (*TrackIndices)[LastVideoTrack].VisibleFrameCount() > 0) {
            FFMS_Track &VideoTrackInfo = (*TrackIndices)[LastVideoTrack];
            LastFrame = VideoTrackInfo.VisibleFrameCount() - 1;
            const FFMS_FrameInfo *Info = VideoTrackInfo.GetFrameInfo(static_cast<size_t>(LastFrame));
            if (Info) {
                LastPTS = Info->PTS;
                LastTimestampMs = av_rescale(Info->PTS, VideoTrackInfo.TB.Num, VideoTrackInfo.TB.Den);
                LastFilePosition = VideoTrackInfo[VideoTrackInfo.RealFrameNumber(static_cast<int>(LastFrame))].FilePos;
            }
            Monitor->SetPosition(LastVideoTrack, LastFrame, LastPTS, LastTimestampMs, LastFilePosition);
        }

        WriteCheckpoint(*TrackIndices, FFMS_CHECKPOINT_COMPLETED, "", true,
            AVContexts, LastValidTS, LastPicStruct);
        Monitor->Stop();
        return TrackIndices.release();
    } catch (FFMS_Exception &e) {
        const int Status = e.GetErrorType() == FFMS_ERROR_CANCELLED ? FFMS_CHECKPOINT_CANCELLED :
            (Monitor->Aborted() ? FFMS_CHECKPOINT_STALLED : FFMS_CHECKPOINT_FAILED);
        robust::WriteFailureReport(RobustOptions, SourceFile, "index scan", Status,
            e.GetErrorMessage(), LastVideoTrack, LastFrame, LastPTS, LastTimestampMs,
            LastFilePosition);
        WriteCheckpoint(*TrackIndices, Status, e.GetErrorMessage(), false,
            AVContexts, LastValidTS, LastPicStruct);
        Monitor->Stop();
        throw;
    } catch (...) {
        robust::WriteFailureReport(RobustOptions, SourceFile, "index scan",
            Monitor->Aborted() ? FFMS_CHECKPOINT_STALLED : FFMS_CHECKPOINT_FAILED,
            "unknown indexing failure", LastVideoTrack, LastFrame, LastPTS,
            LastTimestampMs, LastFilePosition);
        WriteCheckpoint(*TrackIndices, FFMS_CHECKPOINT_FAILED, "unknown indexing failure", false,
            AVContexts, LastValidTS, LastPicStruct);
        Monitor->Stop();
        throw;
    }
}

void FFMS_Indexer::ReadTS(const AVPacket &Packet, int64_t &TS, bool &UseDTS) {
    if (!UseDTS && Packet.pts != AV_NOPTS_VALUE)
        TS = Packet.pts;
    if (TS == AV_NOPTS_VALUE)
        UseDTS = true;
    if (UseDTS && Packet.dts != AV_NOPTS_VALUE)
        TS = Packet.dts;
}
