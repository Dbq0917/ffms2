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

#include "ffms.h"

#include "filelock.h"
#include "indexing.h"
#include "indexstore.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

bool FileExists(const std::string &Path) {
#ifdef _WIN32
    DWORD Attr = GetFileAttributesA(Path.c_str());
    return Attr != INVALID_FILE_ATTRIBUTES && !(Attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat St {};
    return stat(Path.c_str(), &St) == 0 && S_ISREG(St.st_mode);
#endif
}

// Loads and fully validates an index file (magic, ffms2/index/ffmpeg version
// checks plus source signature). Returns nullptr on any failure; callers
// treat that as "rebuild needed" rather than a hard error.
FFMS_Index *TryLoadValidIndex(const std::string &IndexPath, const char *SourceFile) {
    if (!FileExists(IndexPath))
        return nullptr;

    FFMS_ErrorInfo E{};
    char ErrorBuffer[1024];
    E.Buffer = ErrorBuffer;
    E.BufferSize = sizeof(ErrorBuffer);

    FFMS_Index *Index = FFMS_ReadIndex(IndexPath.c_str(), &E);
    if (!Index)
        return nullptr;

    if (!Index->CompareFileSignature(SourceFile)) {
        FFMS_DestroyIndex(Index);
        return nullptr;
    }
    return Index;
}

FFMS_Index *BuildFreshIndex(const char *SourceFile) {
    FFMS_ErrorInfo E{};
    char ErrorBuffer[1024];
    E.Buffer = ErrorBuffer;
    E.BufferSize = sizeof(ErrorBuffer);

    FFMS_Indexer *Indexer = FFMS_CreateIndexer(SourceFile, &E);
    if (!Indexer)
        throw FFMS_Exception(FFMS_ERROR_INDEXING, FFMS_ERROR_FILE_READ,
                             std::string("Could not create indexer: ") + E.Buffer);

    FFMS_TrackTypeIndexSettings(Indexer, FFMS_TYPE_VIDEO, 1, 0);

    FFMS_Index *Index = FFMS_DoIndexing2(Indexer, FFMS_IEH_CLEAR_TRACK, &E);
    if (!Index)
        throw FFMS_Exception(FFMS_ERROR_INDEXING, FFMS_ERROR_UNKNOWN,
                             std::string("Indexing failed: ") + E.Buffer);
    return Index;
}

// Lock files must live on local, writable storage: the source directory can
// be read-only, and network file systems may not support flock. The lock only
// coordinates in-process builds; the atomic rename is what guarantees readers
// never see a partial index, so losing the lock degrades gracefully.
std::string LockFileName(const std::string &IndexPath) {
    std::string Dir;
#ifdef _WIN32
    char Tmp[MAX_PATH];
    DWORD Len = GetTempPathA(MAX_PATH, Tmp);
    Dir = (Len > 0 && Len < MAX_PATH) ? std::string(Tmp) : std::string(".");
#else
    const char *Xdg = std::getenv("XDG_RUNTIME_DIR");
    if (Xdg && *Xdg) {
        Dir = Xdg;
    } else {
        const char *Tmp = std::getenv("TMPDIR");
        if (Tmp && *Tmp)
            Dir = Tmp;
        else
            Dir = "/tmp";
    }
#endif
    if (Dir.back() != '/' && Dir.back() != '\\')
        Dir += '/';
    return Dir + "ffms2-" + IndexStore::CacheKeyFromPath((IndexPath + ".src").c_str()) + ".lock";
}

// Builds while holding the coordination lock, re-checking the cache once the
// lock is held (another process may have published in the meantime).
FFMS_Index *BuildWithLock(const std::string &IndexPath, const char *SourceFile, int LockWaitMs) {
    FileLock Lock(LockFileName(IndexPath));
    bool Locked = Lock.TryLock(LockWaitMs);

    if (Locked) {
        if (FFMS_Index *Existing = TryLoadValidIndex(IndexPath, SourceFile))
            return Existing;
    }

    FFMS_Index *Fresh = BuildFreshIndex(SourceFile);

    if (Locked)
        IndexStore::AtomicWriteIndex(IndexPath.c_str(), Fresh);

    return Fresh;
}

} // namespace

extern "C" FFMS_API(char *) FFMS_GetIndexFileName(const char *SourceFile) {
    if (!SourceFile)
        return nullptr;
    std::string Name = IndexStore::VersionedIndexName(SourceFile);
    char *Out = static_cast<char *>(std::malloc(Name.size() + 1));
    if (!Out)
        return nullptr;
    std::memcpy(Out, Name.c_str(), Name.size() + 1);
    return Out;
}

extern "C" FFMS_API(void) FFMS_FreeString(char *String) {
    std::free(String);
}

extern "C" FFMS_API(FFMS_Index *) FFMS_GetIndexForFile(const char *SourceFile,
                                                        const FFMS_IndexStorage *Storage,
                                                        FFMS_ErrorInfo *ErrorInfo) {
    ClearErrorInfo(ErrorInfo);
    if (!SourceFile || !*SourceFile) {
        if (ErrorInfo) {
            ErrorInfo->ErrorType = FFMS_ERROR_INDEX;
            ErrorInfo->SubType = FFMS_ERROR_INVALID_ARGUMENT;
            std::strncpy(ErrorInfo->Buffer, "SourceFile must not be empty", ErrorInfo->BufferSize - 1);
            ErrorInfo->Buffer[ErrorInfo->BufferSize - 1] = '\0';
        }
        return nullptr;
    }

    int LockWaitMs = 30000;
    const char *LocalCacheDir = nullptr;
    if (Storage) {
        LocalCacheDir = Storage->LocalCacheDir;
        if (Storage->LockWaitMs >= 0)
            LockWaitMs = Storage->LockWaitMs;
    }

    try {
        const std::string VersionedPath = IndexStore::VersionedIndexName(SourceFile);
        const std::string LegacyPath = IndexStore::LegacyIndexName(SourceFile);

        // 1. Fast path: a valid index already exists next to the source.
        //    Both the versioned name and the legacy .ffindex name are tried
        //    so existing deployments migrate without a re-index.
        if (FFMS_Index *Index = TryLoadValidIndex(VersionedPath, SourceFile))
            return Index;
        if (FFMS_Index *Index = TryLoadValidIndex(LegacyPath, SourceFile))
            return Index;

        std::string SourceDir, SourceBase;
        IndexStore::SplitPath(SourceFile, SourceDir, SourceBase);

        // 2. Local cache may already hold a valid index for a read-only
        //    source directory.
        std::string CachePath;
        if (LocalCacheDir && *LocalCacheDir) {
            CachePath = std::string(LocalCacheDir) + "/" +
                        IndexStore::CacheKeyFromPath(SourceFile);
            if (FFMS_Index *Index = TryLoadValidIndex(CachePath, SourceFile))
                return Index;
        }

        // 3. Publish next to the source when writable; otherwise publish to
        //    the local cache; otherwise build in memory only.
        if (IndexStore::DirectoryIsWritable(SourceDir.c_str()))
            return BuildWithLock(VersionedPath, SourceFile, LockWaitMs);

        if (!CachePath.empty() && IndexStore::DirectoryIsWritable(LocalCacheDir))
            return BuildWithLock(CachePath, SourceFile, LockWaitMs);

        return BuildFreshIndex(SourceFile);
    } catch (FFMS_Exception &e) {
        e.CopyOut(ErrorInfo);
        return nullptr;
    } catch (std::exception &e) {
        if (ErrorInfo) {
            ErrorInfo->ErrorType = FFMS_ERROR_INDEX;
            ErrorInfo->SubType = FFMS_ERROR_UNKNOWN;
            std::strncpy(ErrorInfo->Buffer, e.what(), ErrorInfo->BufferSize - 1);
            ErrorInfo->Buffer[ErrorInfo->BufferSize - 1] = '\0';
        }
        return nullptr;
    }
}
