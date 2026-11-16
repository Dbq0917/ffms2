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

#include <string>

// Advisory, process-scoped lock used to coordinate index (re)building across
// many worker processes sharing one source file on network storage.
//
// The lock is best-effort: file systems which do not support locking simply
// make TryLock/Unlock return false instead of throwing, so callers must keep
// working without a lock (the immutable, atomic rename publish step stays
// correct on its own).
class FileLock {
    std::string Filename;
    int FD = -1;
#ifdef _WIN32
    void *Handle = (void *)(long long)-1; // INVALID_HANDLE_VALUE without pulling in windows.h
#endif

    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

public:
    explicit FileLock(std::string Filename);
    ~FileLock();

    // Creates the lock file if needed and acquires an exclusive lock.
    // TimeoutMs <= 0 means a single non-blocking attempt.
    // Returns true when the lock was acquired, false on timeout or when the
    // underlying file system does not support locking.
    bool TryLock(int TimeoutMs = 0);

    void Unlock();

    bool IsLocked() const;
};
