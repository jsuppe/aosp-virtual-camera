#pragma once

#include <vector>

namespace virtualcamera {

// The vector owns every fd. A wait error is not completion. Callers serialize
// access and bound admission of readers; no callback here blocks on a fence.
template <typename Wait, typename Close>
void pruneReadFences(std::vector<int>& fences, Wait wait, Close close) {
    for (auto it = fences.begin(); it != fences.end();) {
        if (wait(*it, 0) == 0) {
            close(*it);
            it = fences.erase(it);
        } else {
            ++it;
        }
    }
}

template <typename Wait, typename Merge, typename Close>
bool consolidateReadFences(std::vector<int>& fences, Wait wait, Merge merge, Close close) {
    pruneReadFences(fences, wait, close);
    while (fences.size() > 1) {
        int combined = merge(fences.front(), fences.back());
        if (combined < 0) return false;  // preserve BOTH dependencies for retry
        close(fences.front());
        close(fences.back());
        fences.front() = combined;
        fences.pop_back();
    }
    return true;
}

template <typename Wait, typename Close>
bool retiredFrameReady(int inflight, std::vector<int>& fences, Wait wait, Close close) {
    pruneReadFences(fences, wait, close);
    return inflight == 0 && fences.empty();
}

template <typename Dup>
bool duplicateAcquireFence(int fd, int* output, Dup duplicate) {
    *output = -1;
    if (fd < 0) return true;
    *output = duplicate(fd);
    return *output >= 0;
}

}  // namespace virtualcamera
