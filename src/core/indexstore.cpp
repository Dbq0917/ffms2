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

#include "indexstore.h"

#include "ffms.h"
#include "indexing.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Must match INDEX_VERSION in indexing.cpp; exposed here for the file name so
// that an index format bump automatically changes the on-disk name.
#define FFMS_INDEX_FILE_VERSION 8

namespace {

std::string TrimTrailingSlashes(const std::string &Path) {
    std::string Result = Path;
    while (Result.size() > 1 && (Result.back() == '/' || Result.back() == '\\'))
        Result.pop_back();
    return Result;
}

} // namespace

namespace IndexStore {

std::string VersionedIndexName(const char *SourceFile) {
    std::ostringstream Name;
    Name << SourceFile << ".ffms2-v" << FFMS_GetVersion()
         << "-idx" << FFMS_INDEX_FILE_VERSION;
    return Name.str();
}

std::string LegacyIndexName(const char *SourceFile) {
    return std::string(SourceFile) + ".ffindex";
}

void SplitPath(const std::string &Path, std::string &Directory, std::string &BaseName) {
    size_t Slash = Path.find_last_of("/\\");
    if (Slash == std::string::npos) {
        Directory = ".";
        BaseName = Path;
    } else {
        Directory = Path.substr(0, Slash);
        if (Directory.empty())
            Directory = "/";
        BaseName = Path.substr(Slash + 1);
    }
}

bool DirectoryIsWritable(const char *Directory) {
    if (!Directory || !*Directory)
        return false;

    std::string Probe(Directory);
    Probe += "/.ffms2-write-probe-";
#ifdef _WIN32
    Probe += "tmp";
    HANDLE H = CreateFileA(Probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (H == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(H);
#else
    Probe += "XXXXXXXXXX";
    std::vector<char> Name(Probe.begin(), Probe.end());
    Name.push_back('\0');
    int FD = mkstemp(Name.data());
    if (FD < 0)
        return false;
    close(FD);
    unlink(Name.data());
#endif
    return true;
}

bool AtomicWriteIndex(const char *Target, const FFMS_Index *Index) {
    if (!Target || !*Target || !Index)
        return false;

    std::string Directory, BaseName;
    SplitPath(Target, Directory, BaseName);
    if (!DirectoryIsWritable(Directory.c_str()))
        return false;

#ifdef _WIN32
    std::string Temp = Target;
    Temp += ".tmp-";
    Temp += std::to_string(GetCurrentProcessId());
#else
    std::string Temp = Target;
    Temp += ".tmp-";
    Temp += std::to_string(getpid());
#endif

    FFMS_ErrorInfo E{};
    char ErrorBuffer[1024];
    E.Buffer = ErrorBuffer;
    E.BufferSize = sizeof(ErrorBuffer);

    if (FFMS_WriteIndex(Temp.c_str(), const_cast<FFMS_Index *>(Index), &E) != FFMS_ERROR_SUCCESS) {
        std::remove(Temp.c_str());
        return false;
    }

#ifdef _WIN32
    // ReplaceFile gives the same atomic swap semantics as rename(2), while
    // allowing an existing target to be replaced.
    if (MoveFileExA(Temp.c_str(), Target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        // Target may not exist yet; try a plain rename in that case.
        if (GetLastError() != ERROR_FILE_NOT_FOUND ||
            MoveFileExA(Temp.c_str(), Target, MOVEFILE_WRITE_THROUGH) == 0) {
            std::remove(Temp.c_str());
            return false;
        }
    }
#else
    if (rename(Temp.c_str(), Target) != 0) {
        std::remove(Temp.c_str());
        return false;
    }
#endif
    return true;
}

std::string CacheKeyFromPath(const char *SourceFile) {
    // FNV-1a over the absolute path; keeps the fallback name short and
    // collision-resistant without pulling in a full hashing dependency.
    const uint64_t FnvOffset = 1469598103934665603ULL;
    const uint64_t FnvPrime = 1099511628211ULL;

    uint64_t Hash = FnvOffset;
    for (const unsigned char *P = reinterpret_cast<const unsigned char *>(SourceFile); *P; ++P) {
        Hash ^= static_cast<uint64_t>(*P);
        Hash *= FnvPrime;
    }

    std::string Dir, Base;
    SplitPath(TrimTrailingSlashes(SourceFile), Dir, Base);

    std::ostringstream Out;
    Out << "ffms2-" << Base << "-" << std::hex << Hash
        << "-v" << FFMS_GetVersion() << "-idx" << FFMS_INDEX_FILE_VERSION;
    return Out.str();
}

} // namespace IndexStore
