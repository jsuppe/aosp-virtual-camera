#include "ReadFenceSet.h"

#include <dirent.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <vector>

// Real Linux fd ownership/exhaustion tests. eventfd is a stand-in dependency,
// NOT a sync_file/GPU fence; actual sync_merge is validated separately on Android.
int openFdCount() {
    DIR* dir = opendir("/proc/self/fd");
    assert(dir);
    int count = 0;
    while (auto* entry = readdir(dir)) {
        if (entry->d_name[0] != '.') ++count;
    }
    closedir(dir);
    return count;
}

int main() {
    const int baseline = openFdCount();
    auto wait = [](int fd, int timeout) {
        pollfd p{fd, POLLIN, 0};
        return poll(&p, 1, timeout) == 1 && p.revents == POLLIN ? 0 : -1;
    };
    auto close = [](int fd) { assert(::close(fd) == 0); };
    for (int iteration = 0; iteration < 1000; ++iteration) {
        int a = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        int b = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        assert(a >= 0 && b >= 0);
        std::vector<int> dependencies{a, b};
        assert(!virtualcamera::consolidateReadFences(dependencies, wait,
                [](int, int) { errno = EMFILE; return -1; }, close));
        assert(dependencies == std::vector<int>({a, b}));
        assert(!virtualcamera::retiredFrameReady(0, dependencies, wait, close));
        assert(eventfd_write(a, 1) == 0);
        assert(!virtualcamera::retiredFrameReady(0, dependencies, wait, close));
        assert(dependencies == std::vector<int>({b}));
        assert(eventfd_write(b, 1) == 0);
        assert(!virtualcamera::retiredFrameReady(1, dependencies, wait, close));
        assert(virtualcamera::retiredFrameReady(0, dependencies, wait, close));
    }
    assert(openFdCount() == baseline);

    int input = eventfd(0, EFD_CLOEXEC);
    assert(input >= 0);
    rlimit original;
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
    rlimit exhausted = original;
    exhausted.rlim_cur = 0;  // process-local: existing descriptors stay open
    assert(setrlimit(RLIMIT_NOFILE, &exhausted) == 0);
    int output = 999;
    bool copied = virtualcamera::duplicateAcquireFence(input, &output,
            [](int fd) { return ::dup(fd); });
    const int duplicationError = errno;
    assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
    assert(!copied && output == -1 && duplicationError == EMFILE);
    close(input);
    assert(openFdCount() == baseline);
    std::puts("PASS: 1000 real-fd merge-failure/retirement cycles, no fd leaks");
    std::puts("PASS: real EMFILE duplication failure rejected; fd limit restored");
}
