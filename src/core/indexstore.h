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

#pragma once

#include <cstdint>
#include <string>

struct FFMS_Index;

namespace IndexStore {

// Versioned, per-source index file name, e.g.
// "/media/cam/clip.mkv.ffms2-v5.1.2-idx8".
std::string VersionedIndexName(const char *SourceFile);

// Legacy unversioned name ("<source>.ffindex"), only checked for reading so
// existing deployments keep working during migration.
std::string LegacyIndexName(const char *SourceFile);

// Replaces Target with the contents of Index atomically. Returns false when
// the target directory is not writable; the caller should then retry against
// the local cache directory. Never leaves a truncated Target behind.
bool AtomicWriteIndex(const char *Target, const FFMS_Index *Index);

// True when a file can be created and renamed inside Directory.
bool DirectoryIsWritable(const char *Directory);

// Splits a path into its directory component and base name.
void SplitPath(const std::string &Path, std::string &Directory, std::string &BaseName);

// Deterministic, filesystem-safe mangling of an absolute source path into a
// single file name, for the local cache directory.
std::string CacheKeyFromPath(const char *SourceFile);

} // namespace IndexStore
