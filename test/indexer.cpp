#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <random>
#include <vector>

#include <ffms.h>
#include <gtest/gtest.h>

#include "data/test.mp4.cpp"
#include "data/vp9_audfirst.webm.cpp"
#include "data/qrvideo_24fps_1elist_1ctts.mov.cpp"
#include "data/qrvideo_24fps_1elist_ends_last_bframe.mov.cpp"
#include "data/qrvideo_24fps_1elist_noctts.mov.cpp"
#include "data/qrvideo_24fps_2elist_elist1_dur_zero.mov.cpp"
#include "data/qrvideo_24fps_2elist_elist1_ends_bframe.mov.cpp"
#include "data/qrvideo_24fps_2s_3elist.mov.cpp"
#include "data/qrvideo_24fps_3elist_1ctts.mov.cpp"
#include "data/qrvideo_24fps_elist_starts_ctts_2ndsample.mov.cpp"
#include "data/qrvideo_stream_shorter_than_movie.mov.cpp"
#include "tests.h"


namespace {

const TestDataMap TestFiles[] = {
    TEST_ENTRY("test.mp4", testmp4data),
    TEST_ENTRY("vp9_audfirst.webm", vp9audfirst),
    TEST_ENTRY("qrvideo_24fps_1elist_1ctts.mov", qrvideo_24fps_1elist_1ctts),
    TEST_ENTRY("qrvideo_24fps_1elist_ends_last_bframe.mov", qrvideo_24fps_1elist_ends_last_bframe),
    TEST_ENTRY("qrvideo_24fps_1elist_noctts.mov", qrvideo_24fps_1elist_noctts),
    TEST_ENTRY("qrvideo_24fps_2elist_elist1_dur_zero.mov", qrvideo_24fps_2elist_elist1_dur_zero),
    TEST_ENTRY("qrvideo_24fps_2elist_elist1_ends_bframe.mov", qrvideo_24fps_2elist_elist1_ends_bframe),
    TEST_ENTRY("qrvideo_24fps_2s_3elist.mov", qrvideo_24fps_2s_3elist),
    TEST_ENTRY("qrvideo_24fps_3elist_1ctts.mov", qrvideo_24fps_3elist_1ctts),
    TEST_ENTRY("qrvideo_24fps_elist_starts_ctts_2ndsample.mov", qrvideo_24fps_elist_starts_ctts_2ndsample),
    TEST_ENTRY("qrvideo_stream_shorter_than_movie.mov", qrvideo_stream_shorter_than_movie),
};

class IndexerTest : public ::testing::TestWithParam<TestDataMap> {
protected:
    virtual void SetUp();
    virtual void TearDown();
    bool DoIndexing(std::string);

    FFMS_Indexer* indexer;
    FFMS_Index* index;
    int video_track_idx;
    FFMS_VideoSource* video_source;
    const FFMS_VideoProperties* VP;

    FFMS_ErrorInfo E;
    char ErrorMsg[1024];

    std::string SamplesDir;
};

int FFMS_CC CancelLateInIndexing(int64_t Current, int64_t Total, void *Private) {
    bool *Cancelled = static_cast<bool *>(Private);
    if (!*Cancelled && Total > 0 && Current * 100 / Total >= 40) {
        *Cancelled = true;
        return 1;
    }
    return 0;
}

void IndexerTest::SetUp() {
    indexer = nullptr;
    index = nullptr;
    video_track_idx = -1;
    video_source = nullptr;
    VP = nullptr;
    E.Buffer = ErrorMsg;
    E.BufferSize = sizeof(ErrorMsg);
    SamplesDir = STRINGIFY(SAMPLES_DIR);

    FFMS_Init(0,0);
}

void IndexerTest::TearDown() {
    FFMS_DestroyIndex(index);
    FFMS_DestroyVideoSource(video_source);
}

bool IndexerTest::DoIndexing(std::string file_name) {
    indexer = FFMS_CreateIndexer(file_name.c_str(), &E);
    NULL_CHECK(indexer);
    FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_VIDEO, 1, 0);

    index = FFMS_DoIndexing2(indexer, 0, &E);
    NULL_CHECK(index);

    video_track_idx = FFMS_GetFirstTrackOfType(index, FFMS_TYPE_VIDEO, &E);
    EXPECT_GE(0, video_track_idx);

    video_source = FFMS_CreateVideoSource(file_name.c_str(), video_track_idx, index, 1, FFMS_SEEK_NORMAL, &E);
    NULL_CHECK(video_source);

    VP = FFMS_GetVideoProperties(video_source); // Can't fail

    return true;
}

TEST_P(IndexerTest, ValidateFrameCount) {
    TestDataMap P = GetParam();
    std::string FilePath = SamplesDir + "/" + P.Filename;

    ASSERT_TRUE(DoIndexing(FilePath));

    ASSERT_EQ(P.TestDataLen, VP->NumFrames);
}

TEST_P(IndexerTest, ReverseAccessingFrame) {
    TestDataMap P = GetParam();
    std::string FilePath = SamplesDir + "/" + P.Filename;

    ASSERT_TRUE(DoIndexing(FilePath));
    for (int i = VP->NumFrames - 1; i > 0; i--) {
        std::stringstream ss;
        ss << "Testing Frame: " << i;
        SCOPED_TRACE(ss.str());

        FFMS_Track *track = FFMS_GetTrackFromIndex(index, video_track_idx);
        const FFMS_FrameInfo *info = FFMS_GetFrameInfo(track, i);

        const FFMS_Frame* frame = FFMS_GetFrame(video_source, i, &E);
        ASSERT_NE(nullptr, frame);
        ASSERT_TRUE(CheckFrame(frame, info, &P.TestData[i]));
    }
}

TEST_P(IndexerTest, RandomAccessingFrame) {
    TestDataMap P = GetParam();
    std::string FilePath = SamplesDir + "/" + P.Filename;

    ASSERT_TRUE(DoIndexing(FilePath));

    std::vector<int> FrameNums;
    for (int i = 0; i < VP->NumFrames; i++)
        FrameNums.push_back(i);

    std::random_device Device;
    std::mt19937 Gen(Device());

    std::shuffle(FrameNums.begin(), FrameNums.end(), Gen);

    for (int i = 0; i < VP->NumFrames; i++) {
        int num = FrameNums[i];

        std::stringstream ss;
        ss << "Testing Frame: " << num;
        SCOPED_TRACE(ss.str());

        FFMS_Track *track = FFMS_GetTrackFromIndex(index, video_track_idx);
        const FFMS_FrameInfo *info = FFMS_GetFrameInfo(track, num);

        const FFMS_Frame* frame = FFMS_GetFrame(video_source, num, &E);
        ASSERT_NE(nullptr, frame);
        ASSERT_TRUE(CheckFrame(frame, info, &P.TestData[num]));
    }
}

INSTANTIATE_TEST_CASE_P(ValidateIndexer, IndexerTest, ::testing::ValuesIn(TestFiles));

TEST(IndexerRobust, CheckpointResumeAndOutputDuration) {
    const std::string FilePath = std::string(STRINGIFY(SAMPLES_DIR)) + "/test.mp4";
    const std::string CheckpointFile = FilePath + ".ffms2-robust-test.checkpoint.txt";
    const std::string ReportFile = CheckpointFile + ".report.txt";
    std::remove(CheckpointFile.c_str());
    std::remove(ReportFile.c_str());

    char ErrorMsg[1024] = {};
    FFMS_ErrorInfo E = {};
    E.Buffer = ErrorMsg;
    E.BufferSize = sizeof(ErrorMsg);

    FFMS_RobustOptions Options = {};
    Options.EnableStallDetection = 1;
    Options.StallTimeoutMs = 5000;
    Options.EnableCheckpoint = 1;
    Options.CheckpointIntervalMs = 1000;
    Options.CheckpointFile = CheckpointFile.c_str();
    Options.ReportFile = ReportFile.c_str();

    FFMS_Init(0, 0);

    FFMS_Indexer *Indexer = FFMS_CreateIndexerWithOptions(FilePath.c_str(), nullptr, 0, &Options, &E);
    ASSERT_NE(nullptr, Indexer) << ErrorMsg;
    bool Cancelled = false;
    FFMS_SetProgressCallback(Indexer, CancelLateInIndexing, &Cancelled);
    FFMS_Index *Index = FFMS_DoIndexing2(Indexer, FFMS_IEH_CLEAR_TRACK, &E);
    ASSERT_EQ(nullptr, Index);
    ASSERT_TRUE(Cancelled);

    FFMS_CheckpointInfo Info = {};
    ASSERT_EQ(FFMS_ERROR_SUCCESS, FFMS_ReadCheckpoint(CheckpointFile.c_str(), &Info, &E)) << ErrorMsg;
    ASSERT_EQ(FFMS_CHECKPOINT_CANCELLED, Info.Status);
    ASSERT_GE(Info.LastFrame, 0);
    const int64_t CancelledFrame = Info.LastFrame;

    Indexer = FFMS_CreateIndexerWithOptions(FilePath.c_str(), nullptr, 0, &Options, &E);
    ASSERT_NE(nullptr, Indexer) << ErrorMsg;
    Index = FFMS_DoIndexing2(Indexer, FFMS_IEH_CLEAR_TRACK, &E);
    ASSERT_NE(nullptr, Index) << ErrorMsg;

    ASSERT_EQ(FFMS_ERROR_SUCCESS, FFMS_ReadCheckpoint(CheckpointFile.c_str(), &Info, &E)) << ErrorMsg;
    ASSERT_EQ(FFMS_CHECKPOINT_COMPLETED, Info.Status);
    ASSERT_GT(Info.LastFrame, CancelledFrame);

    int VideoTrack = FFMS_GetFirstIndexedTrackOfType(Index, FFMS_TYPE_VIDEO, &E);
    ASSERT_GE(VideoTrack, 0) << ErrorMsg;
    FFMS_VideoSource *Video = FFMS_CreateVideoSourceWithOptions(FilePath.c_str(), VideoTrack,
        Index, 1, FFMS_SEEK_NORMAL, &Options, &E);
    ASSERT_NE(nullptr, Video) << ErrorMsg;
    const int Frames = FFMS_GetNumFrames(FFMS_GetTrackFromVideo(Video));
    for (int Frame = 0; Frame < Frames; ++Frame)
        ASSERT_NE(nullptr, FFMS_GetFrame(Video, Frame, &E)) << "frame " << Frame << ": " << ErrorMsg;
    ASSERT_GT(FFMS_GetVideoProperties(Video)->LastEndTime, 0.0);

    FFMS_DestroyVideoSource(Video);
    FFMS_DestroyIndex(Index);
    std::remove(CheckpointFile.c_str());
    std::remove(ReportFile.c_str());
}

} //namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
