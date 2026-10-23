#include "AcquireFenceWait.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <initializer_list>

// These exercise the production decision helper, not a mock EGL driver.
int main() {
    int cases = 0;
    for (const int fd : {-1, -2}) {
        assert(virtualcamera::acquireFenceReady(fd,
                [](int) { assert(false); return false; },
                [](int, int) { assert(false); return -1; }));
        ++cases;
    }
    for (const int fd : {0, 42}) {
        int gpuCalls = 0;
        assert(virtualcamera::acquireFenceReady(fd,
                [&](int actual) { assert(actual == fd); ++gpuCalls; return true; },
                [](int, int) { assert(false); return -1; }));
        assert(gpuCalls == 1);
        ++cases;
    }
    for (const int result : {0, -1}) {
        int gpuCalls = 0, cpuCalls = 0;
        const bool ready = virtualcamera::acquireFenceReady(42,
                [&](int fd) { assert(fd == 42); ++gpuCalls; return false; },
                [&](int fd, int timeout) {
                    assert(fd == 42 && timeout == 100);
                    ++cpuCalls;
                    return result;
                });
        assert(ready == (result == 0));
        assert(gpuCalls == 1 && cpuCalls == 1);
        ++cases;
    }
    for (const int error : {ETIME, EINVAL, EINTR}) {
        assert(!virtualcamera::acquireFenceReady(42, [](int) { return false; },
                [&](int, int) { errno = error; return -1; }));
        ++cases;
    }
    std::printf("PASS: %d acquire-fence decision cases\n", cases);
}
