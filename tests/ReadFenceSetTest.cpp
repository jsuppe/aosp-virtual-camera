#include "ReadFenceSet.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

// Deterministic dependency/fd fault model. Tests execute the production helper;
// these are not a GPU driver or Binder integration test.
struct Fences {
    int next = 10;
    bool failMerge = false;
    bool failWait = false;
    std::map<int, std::set<int>> live;
    std::set<int> signaled;
    int make(std::set<int> dependencies) {
        int fd = next++;
        live.emplace(fd, std::move(dependencies));
        return fd;
    }
    int wait(int fd, int timeout) {
        assert(timeout == 0 && live.count(fd));
        if (failWait) return -1;
        for (int dependency : live.at(fd)) {
            if (!signaled.count(dependency)) return -1;
        }
        return 0;
    }
    void close(int fd) { assert(live.erase(fd) == 1); }
    int merge(int a, int b) {
        assert(a != b && live.count(a) && live.count(b));
        if (failMerge) return -1;
        auto dependencies = live.at(a);
        dependencies.insert(live.at(b).begin(), live.at(b).end());
        return make(std::move(dependencies));
    }
    bool consolidate(std::vector<int>& f) {
        return virtualcamera::consolidateReadFences(f,
                [&](int fd, int ms) { return wait(fd, ms); },
                [&](int a, int b) { return merge(a, b); },
                [&](int fd) { close(fd); });
    }
    bool retired(int readers, std::vector<int>& f) {
        return virtualcamera::retiredFrameReady(readers, f,
                [&](int fd, int ms) { return wait(fd, ms); },
                [&](int fd) { close(fd); });
    }
};

int main() {
    int cases = 0;
    {
        Fences ops;
        std::vector<int> f;
        assert(ops.consolidate(f) && ops.retired(0, f)); ++cases;
        assert(!ops.retired(1, f)); ++cases;  // late reader has no fence yet
    }
    {
        Fences ops;
        int a = ops.make({1}), b = ops.make({2});
        std::vector<int> f{a, b};
        ops.failMerge = true;
        assert(!ops.consolidate(f) && f == std::vector<int>({a, b}));
        assert(ops.live.size() == 2); ++cases;
        assert(!ops.retired(0, f) && ops.live.size() == 2); ++cases;
        ops.signaled.insert(1);
        assert(ops.consolidate(f) && f == std::vector<int>({b}));
        assert(ops.live.count(b) && !ops.live.count(a)); ++cases;
        assert(!ops.retired(0, f)); ++cases;
        ops.signaled.insert(2);
        assert(ops.retired(0, f) && ops.live.empty()); ++cases;
    }
    {
        Fences ops;
        std::vector<int> f{ops.make({1}), ops.make({2}), ops.make({3})};
        assert(ops.consolidate(f) && f.size() == 1);
        assert(ops.live.at(f.front()) == std::set<int>({1, 2, 3})); ++cases;
        ops.signaled = {1, 3};
        assert(!ops.retired(0, f)); ++cases;
        ops.signaled.insert(2);
        assert(!ops.retired(1, f) && f.empty()); ++cases;
        // Last reader contributes a late completion dependency after close.
        f.push_back(ops.make({4}));
        assert(!ops.retired(0, f)); ++cases;
        ops.signaled.insert(4);
        assert(ops.retired(0, f) && ops.live.empty()); ++cases;
    }
    {
        Fences ops;
        std::vector<int> f{ops.make({1}), ops.make({2})};
        ops.failMerge = true;
        assert(!ops.consolidate(f));
        ops.failMerge = false;
        assert(ops.consolidate(f) && ops.live.size() == 1); ++cases;
        ops.failWait = true;
        ops.signaled = {1, 2};
        assert(!ops.retired(0, f) && ops.live.size() == 1); ++cases;
        ops.failWait = false;
        assert(ops.retired(0, f) && ops.live.empty()); ++cases;
    }
    {
        Fences ops;
        std::vector<int> first{ops.make({1})}, second{ops.make({2})};
        ops.signaled = {1};
        assert(ops.retired(0, first) && !ops.retired(0, second)); ++cases;
        ops.signaled.insert(2);
        assert(ops.retired(0, second) && ops.live.empty()); ++cases;
    }
    {
        Fences ops;
        std::vector<int> f{ops.make({1}), ops.make({2}), ops.make({3})};
        int merges = 0;
        assert(!virtualcamera::consolidateReadFences(f,
                [&](int fd, int ms) { return ops.wait(fd, ms); },
                [&](int a, int b) { return ++merges == 2 ? -1 : ops.merge(a, b); },
                [&](int fd) { ops.close(fd); }));
        assert(f.size() == 2 && ops.live.size() == 2);
        std::set<int> retained;
        for (int fd : f) retained.insert(ops.live.at(fd).begin(), ops.live.at(fd).end());
        assert(retained == std::set<int>({1, 2, 3})); ++cases;
        ops.signaled = {1, 2, 3};
        assert(ops.retired(0, f) && ops.live.empty()); ++cases;
    }
    for (int input : {-1, 0, 42}) {
        int output = 999, calls = 0;
        assert(virtualcamera::duplicateAcquireFence(input, &output,
                [&](int fd) { assert(fd == input); ++calls; return 77; }));
        assert(calls == (input < 0 ? 0 : 1));
        assert(output == (input < 0 ? -1 : 77)); ++cases;
    }
    {
        int output = 999;
        assert(!virtualcamera::duplicateAcquireFence(42, &output, [](int) { return -1; }));
        assert(output == -1); ++cases;
    }
    std::printf("PASS: %d read-fence ownership/fault cases\n", cases);
}
