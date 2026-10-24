#pragma once

namespace virtualcamera {

// No source read is permitted unless the GPU accepted the dependency or a
// bounded CPU wait completed. Callbacks make the decision testable on a host
// without EGL or Android fence objects. Neither callback consumes the input fd.
template <typename GpuWait, typename CpuWait>
bool acquireFenceReady(int fd, GpuWait gpuWait, CpuWait cpuWait) {
    if (fd < 0) return true;
    if (gpuWait(fd)) return true;
    return cpuWait(fd, 100) == 0;
}

}  // namespace virtualcamera
