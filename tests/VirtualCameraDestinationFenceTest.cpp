#include "SessionTestHarness.h"

int main() {
    int cases = 0;
    {
        Fixture f(1); f.producer.submit();
        ndk::ScopedFileDescriptor fence(eventfd(0, EFD_CLOEXEC)); assert(fence.get() >= 0);
        const auto before = f.pixels();
        int32_t accepted = -1;
        assert(f.run(f.request({fence.get()}), &accepted).isOk());
        if (!f.failedUntouched(before, accepted)) {
            std::fprintf(stderr, "FAIL: unready destination was written or returned as success (shutters=%d errors=%d)\n",
                    f.callback->shutters, f.callback->errors);
            return 1;  // baseline reproducer, not a HAL-process crash
        }
        assert(f.callback->buffers[0].fence.get() >= 0);
        assert(eventfd_write(fence.get(), 1) == 0);
        assert(sync_wait(f.callback->buffers[0].fence.get(), 0) == 0);
        ++cases;
        f.callback->reset();
        auto request = f.request({fence.get()});
        request.settings.metadata.clear();  // accepted failed request retains settings
        request.outputBuffers[0].buffer = {};  // accepted ERROR also owns the cache entry
        if (!f.run(std::move(request), &accepted).isOk() || accepted != 1) {
            std::fputs("FAIL: accepted failed request did not cache its original buffer handle\n", stderr);
            return 1;
        }
        assert(f.callback->errors == 0 && f.callback->shutters == 1 && f.callback->results == 1);
        assert(!f.callback->emptyMetadata && f.pixels() != before);
        ++cases;
        f.callback->reset();
        f.pixels(true);
        ndk::ScopedFileDescriptor blocked(eventfd(0, EFD_CLOEXEC)); assert(blocked.get() >= 0);
        request = f.request({blocked.get()});
        request.outputBuffers[0].buffer = {};  // cached buffer path must also wait
        assert(f.run(std::move(request), &accepted).isOk());
        assert(f.failedUntouched(before, accepted));
        ++cases;
    }
    {
        Fixture f(3); f.producer.submit();
        ndk::ScopedFileDescriptor ready(eventfd(1, EFD_CLOEXEC));
        ndk::ScopedFileDescriptor failed(eventfd(0, EFD_CLOEXEC));
        ndk::ScopedFileDescriptor unvisited(eventfd(0, EFD_CLOEXEC));
        assert(ready.get() >= 0 && failed.get() >= 0 && unvisited.get() >= 0);
        const auto before = f.pixels(); int32_t accepted = -1;
        assert(f.run(f.request({ready.get(), failed.get(), unvisited.get()}), &accepted).isOk());
        assert(f.failedUntouched(before, accepted));
        assert(f.callback->buffers[0].fence.get() == -1);
        assert(f.callback->buffers[1].fence.get() >= 0 && f.callback->buffers[2].fence.get() >= 0);
        assert(eventfd_write(failed.get(), 1) == 0);
        assert(sync_wait(f.callback->buffers[1].fence.get(), 0) == 0);
        assert(sync_wait(f.callback->buffers[2].fence.get(), 0) != 0);
        assert(eventfd_write(unvisited.get(), 1) == 0);
        assert(sync_wait(f.callback->buffers[2].fence.get(), 0) == 0);
        ++cases;
    }
    {
        Fixture f(1); f.producer.submit(); const auto before = f.pixels();
        ndk::ScopedFileDescriptor fence(eventfd(0, EFD_CLOEXEC)); assert(fence.get() >= 0);
        std::thread producer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(f.pixels() == before);  // no write before dependency signals
            assert(eventfd_write(fence.get(), 1) == 0);
        });
        int32_t accepted = -1;
        assert(f.run(f.request({fence.get()}), &accepted).isOk());
        producer.join();
        assert(accepted == 1 && f.callback->errors == 0 && f.pixels() != before);
        ++cases;
    }
    for (int malformed = 0; malformed < 3; ++malformed) {
        Fixture f(1); f.producer.submit(); const auto before = f.pixels();
        auto request = f.request({-1});
        auto& fence = request.outputBuffers[0].acquireFence;
        if (malformed == 0) fence.ints.push_back(1);
        if (malformed == 1) fence.fds.emplace_back(-1);
        if (malformed == 2) {
            fence.fds.emplace_back(eventfd(0, EFD_CLOEXEC));
            fence.fds.emplace_back(eventfd(0, EFD_CLOEXEC));
            assert(fence.fds[0].get() >= 0 && fence.fds[1].get() >= 0);
        }
        int32_t accepted = -1;
        auto status = f.run(std::move(request), &accepted);
        assert(!status.isOk() && status.getServiceSpecificError() == static_cast<int>(CameraStatus::ILLEGAL_ARGUMENT));
        assert(accepted == 0 && f.callback->events.empty() && f.pixels() == before);
        ++cases;
    }
    {
        Fixture f(1); f.producer.submit(); const auto before = f.pixels();
        ndk::ScopedFileDescriptor fence(eventfd(0, EFD_CLOEXEC)); assert(fence.get() >= 0);
        auto request = f.request({fence.get()});
        rlimit original; assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
        rlimit exhausted = original; exhausted.rlim_cur = 0;
        assert(setrlimit(RLIMIT_NOFILE, &exhausted) == 0);
        int32_t accepted = -1;
        auto status = f.run(std::move(request), &accepted);
        assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
        assert(!status.isOk() && status.getServiceSpecificError() == static_cast<int>(CameraStatus::INTERNAL_ERROR));
        assert(accepted == 0 && f.callback->events.empty() && f.pixels() == before);
        assert(fcntl(fence.get(), F_GETFD) >= 0);
        ++cases;
    }
    {
        Fixture f(1); f.producer.submit(); const auto before = f.pixels();
        int pipeFds[2]; assert(pipe2(pipeFds, O_CLOEXEC) == 0);
        assert(::close(pipeFds[0]) == 0);
        ndk::ScopedFileDescriptor errorFence(pipeFds[1]);
        int32_t accepted = -1;
        assert(f.run(f.request({errorFence.get()}), &accepted).isOk());
        assert(f.failedUntouched(before, accepted));
        assert(f.callback->buffers[0].fence.get() >= 0);
        assert(sync_wait(f.callback->buffers[0].fence.get(), 0) != 0 && errno == EINVAL);
        ++cases;
    }
    {
        Fixture f(3); f.producer.submit(); const auto before = f.pixels();
        ndk::ScopedFileDescriptor fence(eventfd(0, EFD_CLOEXEC)); assert(fence.get() >= 0);
        auto request = f.request({fence.get(), fence.get(), fence.get()});
        int freeSlot = ::dup(fence.get()); assert(freeSlot >= 0);
        assert(::close(freeSlot) == 0);
        rlimit original; assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
        rlimit oneDup = original; oneDup.rlim_cur = freeSlot + 1;
        assert(setrlimit(RLIMIT_NOFILE, &oneDup) == 0);
        int32_t accepted = -1;
        auto status = f.run(std::move(request), &accepted);
        assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
        assert(!status.isOk() && status.getServiceSpecificError() == static_cast<int>(CameraStatus::INTERNAL_ERROR));
        assert(accepted == 0 && f.callback->events.empty() && f.pixels() == before);
        assert(fcntl(freeSlot, F_GETFD) == -1 && errno == EBADF);  // partial dup cleaned up
        assert(fcntl(fence.get(), F_GETFD) >= 0);
        ++cases;
    }
    {
        Fixture f(1); f.producer.submit(); const auto before = f.pixels();
        auto request = f.request({-1});
        // Structurally a handle, but not a gralloc buffer: import must fail
        // without closing the fds borrowed from the still-live request.
        request.outputBuffers[0].buffer = {};
        request.outputBuffers[0].buffer.fds.emplace_back(eventfd(0, EFD_CLOEXEC));
        int borrowed = request.outputBuffers[0].buffer.fds[0].get(); assert(borrowed >= 0);
        std::vector<device::CaptureRequest> requests;
        requests.push_back(std::move(request));
        int32_t accepted = -1;
        auto status = f.session->processCaptureRequest(requests, {}, &accepted);
        assert(!status.isOk() && status.getServiceSpecificError() == static_cast<int>(CameraStatus::ILLEGAL_ARGUMENT));
        assert(accepted == 0 && f.callback->events.empty() && f.pixels() == before);
        assert(fcntl(borrowed, F_GETFD) >= 0);
        ++cases;
    }
    std::printf("PASS: %d destination-fence/callback/pixel-integrity cases\n", cases);
}
