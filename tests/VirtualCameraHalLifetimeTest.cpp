// Exercises actual HAL methods with real gralloc handles on Android. No GPU
// work is submitted: delayed-reader tests intentionally delay releaseFrame().
#include "VirtualCameraStableHal.h"

#include <aidlcommonsupport/NativeHandle.h>
#include <vndk/hardware_buffer.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

using aidl::android::hardware::camera::provider::implementation::VirtualCameraStableHal;

int main() {
    AHardwareBuffer_Desc description{};
    description.width = description.height = 16;
    description.layers = 1;
    description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    description.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* source = nullptr;
    assert(AHardwareBuffer_allocate(&description, &source) == 0 && source);
    AHardwareBuffer_describe(source, &description);
    auto handle = ::android::dupToAidl(AHardwareBuffer_getNativeHandle(source));
    auto hal = ndk::SharedRefBase::make<VirtualCameraStableHal>();
    ndk::ScopedFileDescriptor noFence, retired;
    auto queue = [&](int slot, int64_t timestamp, const ndk::ScopedFileDescriptor& fence) {
        return hal->queueFrameForCamera(slot, handle, description.width, description.height,
                description.stride, description.format, description.usage,
                timestamp, fence, &retired);
    };
    int cases = 0;

    assert(queue(0, 100, noFence).isOk());
    int64_t timestamp = 0;
    int acquire = -1;
    AHardwareBuffer* reader = hal->acquireLatest(0, &timestamp, &acquire);
    assert(reader && timestamp == 100 && acquire == -1);
    auto begin = std::chrono::steady_clock::now();
    auto rejected = queue(0, 200, noFence);
    assert(!rejected.isOk() && rejected.getServiceSpecificError() == 4);
    assert(std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(150));
    AHardwareBuffer* retained = hal->acquireLatest(0, &timestamp, &acquire);
    assert(retained == reader && timestamp == 100);
    hal->releaseFrame(0, retained, -1);
    hal->releaseFrame(0, reader, -1);
    assert(queue(0, 200, noFence).isOk());
    ++cases;

    reader = hal->acquireLatest(0, &timestamp, &acquire);
    assert(reader && timestamp == 200);
    assert(hal->setCameraPresent(0, false).isOk());
    assert(!hal->hasFrame(0) && !hal->acquireLatest(0, &timestamp, &acquire));
    rejected = queue(0, 300, noFence);
    assert(!rejected.isOk() && rejected.getServiceSpecificError() == 4);
    hal->releaseFrame(0, reader, -1);  // late reader still finds retiring frame
    assert(queue(0, 300, noFence).isOk());
    ++cases;

    rejected = queue(-1, 400, noFence);
    assert(!rejected.isOk() && rejected.getServiceSpecificError() == 3);
    ++cases;

    std::vector<AHardwareBuffer*> readers;
    for (int i = 0; i < 16; ++i) {
        reader = hal->acquireLatest(0, &timestamp, &acquire);
        assert(reader);
        readers.push_back(reader);
    }
    assert(!hal->acquireLatest(0, &timestamp, &acquire));
    for (auto* pending : readers) hal->releaseFrame(0, pending, -1);
    assert(queue(0, 350, noFence).isOk());
    ++cases;

    reader = hal->acquireLatest(0, &timestamp, &acquire);
    assert(reader && queue(1, 360, noFence).isOk());
    assert(hal->setCameraPresent(0, false).isOk());
    rejected = queue(0, 370, noFence);
    assert(!rejected.isOk() && rejected.getServiceSpecificError() == 4);
    assert(queue(1, 380, noFence).isOk());  // other slot is unaffected
    hal->releaseFrame(0, reader, -1);
    assert(queue(0, 390, noFence).isOk());
    assert(hal->setCameraPresent(1, false).isOk());
    ++cases;

    ndk::ScopedFileDescriptor inputFence(eventfd(0, EFD_CLOEXEC));
    assert(inputFence.get() >= 0);
    rlimit original;
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
    rlimit exhausted = original;
    exhausted.rlim_cur = 0;
    assert(setrlimit(RLIMIT_NOFILE, &exhausted) == 0);
    rejected = queue(0, 400, inputFence);
    auto rejectedV2 = hal->queueFrameFenced(handle, description.width, description.height,
            description.stride, description.format, description.usage,
            400, inputFence, &retired);
    assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
    assert(!rejected.isOk() && rejected.getServiceSpecificError() == 5);
    ++cases;
    assert(!rejectedV2.isOk() && rejectedV2.getServiceSpecificError() == 5);
    ++cases;

    assert(queue(0, 500, inputFence).isOk());
    assert(setrlimit(RLIMIT_NOFILE, &exhausted) == 0);
    reader = hal->acquireLatest(0, &timestamp, &acquire);
    assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
    assert(!reader && acquire == -1);
    assert(hal->setCameraPresent(0, false).isOk());
    // A failed acquisition must not leak an in-flight reader reservation.
    assert(queue(0, 600, noFence).isOk());
    ++cases;

    assert(hal->setCameraPresent(0, false).isOk());
    AHardwareBuffer_release(source);
    std::printf("PASS: %d Android HAL lifetime/backpressure/dup-failure cases\n", cases);
}
