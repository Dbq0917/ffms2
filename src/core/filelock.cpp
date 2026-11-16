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

#include "filelock.h"

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

FileLock::FileLock(std::string Filename) : Filename(std::move(Filename)) {
}

FileLock::~FileLock() {
    Unlock();
}

bool FileLock::IsLocked() const {
#ifdef _WIN32
    return Handle != (void *)(long long)-1;
#else
    return FD >= 0;
#endif
}

#ifndef _WIN32
static bool FlockSupported(int Err) {
    // EOPNOTSUPP/ENOSYS: network FS (or kernel) without locking support.
    // EINVAL here can also be reported by some FUSE/SMB mounts.
    return Err != EOPNOTSUPP && Err != ENOSYS && Err != EINVAL;
}
#endif

bool FileLock::TryLock(int TimeoutMs) {
    if (IsLocked())
        return true;

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs > 0 ? TimeoutMs : 0);

#ifdef _WIN32
    HANDLE H = CreateFileA(Filename.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (H == INVALID_HANDLE_VALUE)
        return false;

    do {
        OVERLAPPED Ov{};
        Ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        bool Acquired = Ov.hEvent != nullptr &&
                        LockFileEx(H, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                                   0, MAXDWORD, MAXDWORD, &Ov);
        if (Ov.hEvent)
            CloseHandle(Ov.hEvent);
        if (Acquired) {
            Handle = H;
            return true;
        }
        DWORD Err = GetLastError();
        if (Err != ERROR_LOCK_VIOLATION) {
            CloseHandle(H);
            return false;
        }
        if (TimeoutMs <= 0) {
            CloseHandle(H);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < Deadline);

    CloseHandle(H);
    return false;
#else
    while (true) {
        int NewFD = open(Filename.c_str(), O_RDWR | O_CREAT, 0644);
        if (NewFD < 0)
            return false;

        if (flock(NewFD, LOCK_EX | LOCK_NB) == 0) {
            FD = NewFD;
            return true;
        }

        int Err = errno;
        close(NewFD);
        if (!FlockSupported(Err))
            return false;
        if (Err != EWOULDBLOCK && Err != EAGAIN)
            return false;
        if (TimeoutMs <= 0 || std::chrono::steady_clock::now() >= Deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

void FileLock::Unlock() {
#ifdef _WIN32
    if (Handle != (void *)(long long)-1) {
        UnlockFile((HANDLE)Handle, 0, MAXDWORD, MAXDWORD, nullptr);
        CloseHandle((HANDLE)Handle);
        Handle = (void *)(long long)-1;
    }
#else
    if (FD >= 0) {
        flock(FD, LOCK_UN | LOCK_NB);
        close(FD);
        FD = -1;
    }
#endif
}
