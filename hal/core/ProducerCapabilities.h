#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace virtualcamera {
// Implementation resource ceilings, not fixed product modes. Keep Java validation in sync.
struct ProducerMode {
    int id = 0, width = 0, height = 0, minFps = 0, maxFps = 0;
    std::vector<int> formats;
    bool downscale = false;
};
struct ProducerCapabilities {
    int64_t registration = 0;
    int facing = 2, orientation = 0, maxStreams = 3;
    std::vector<ProducerMode> modes;
    bool valid() const {
        if (registration <= 0 || modes.empty() || modes.size() > 32 || facing < 0 || facing > 2 ||
            (orientation != 0 && orientation != 90 && orientation != 180 && orientation != 270) ||
            maxStreams < 1 || maxStreams > 3) return false;
        std::set<int> ids;
        for (const auto& m : modes) {
            if (m.id <= 0 || !ids.insert(m.id).second || m.width < 16 || m.height < 16 ||
                m.width > 8192 || m.height > 8192 || (m.width & 1) || (m.height & 1) ||
                int64_t(m.width) * m.height > 3840LL * 2160 || m.minFps < 1 ||
                m.maxFps < m.minFps || m.maxFps > 120 || m.formats.empty() || m.formats.size() > 4) return false;
            std::set<int> formats;
            for (int f : m.formats) if ((f != 1 && f != 0x22 && f != 0x23 && f != 0x21) ||
                    !formats.insert(f).second) return false;
        }
        return true;
    }
    struct Output { int format, width, height; };
    std::pair<int,int> defaultRange() const {
        int commonLow = 1, commonHigh = 120, globalLow = 120, globalHigh = 1;
        for (const auto& m : modes) {
            commonLow = std::max(commonLow, m.minFps);
            commonHigh = std::min(commonHigh, m.maxFps);
            globalLow = std::min(globalLow, m.minFps);
            globalHigh = std::max(globalHigh, m.maxFps);
        }
        if (commonLow <= commonHigh) {
            int fps = std::clamp(30, commonLow, commonHigh);
            return {fps, fps};
        }
        return {globalLow, globalHigh};
    }
    std::set<std::pair<int,int>> fpsRanges() const {
        std::set<std::pair<int,int>> ranges;
        ranges.insert(defaultRange());
        for(const auto& m:modes) {
            ranges.insert({m.minFps,m.maxFps}); ranges.insert({m.minFps,m.minFps}); ranges.insert({m.maxFps,m.maxFps});
            if(m.minFps<=30&&m.maxFps>=30) {
                ranges.insert({30,30});ranges.insert({m.minFps,30});ranges.insert({30,m.maxFps});
            }
        }
        return ranges;
    }
    bool advertised(const Output& o) const {
        for (const auto& m : modes) if (m.width == o.width && m.height == o.height &&
            std::find(m.formats.begin(), m.formats.end(), o.format) != m.formats.end()) return true;
        return false;
    }
    // All outputs must come from ONE declared source mode. No cross-product of unrelated modes.
    const ProducerMode* select(const std::vector<Output>& outputs, int low = 0, int high = 0) const {
        if (!valid() || outputs.empty() || outputs.size() > size_t(maxStreams)) return nullptr;
        if(high && !fpsRanges().count({low,high}))return nullptr;
        int jpeg = 0;
        for (const auto& o : outputs) { if (!advertised(o)) return nullptr; if (o.format == 0x21) ++jpeg; }
        if (jpeg > 1 || int(outputs.size()) - jpeg > 2) return nullptr;
        const ProducerMode* best = nullptr;
        for (const auto& m : modes) {
            if (high && (low < 1 || low > high || high < m.minFps || low > m.maxFps)) continue;
            bool fits = true;
            for (const auto& o : outputs) {
                if (std::find(m.formats.begin(), m.formats.end(), o.format) == m.formats.end() ||
                    o.width > m.width || o.height > m.height ||
                    ((o.width != m.width || o.height != m.height) &&
                     (!m.downscale || int64_t(o.width) * m.height != int64_t(o.height) * m.width))) fits = false;
            }
            if (fits && (!best || int64_t(m.width)*m.height < int64_t(best->width)*best->height ||
                    (m.width == best->width && m.height == best->height && m.id < best->id))) best = &m;
        }
        return best;
    }
    static ProducerCapabilities diagnostic() {
        ProducerCapabilities c; c.registration = 1;
        for (auto size : {Output{1,3840,2160}, Output{1,1920,1080}, Output{1,1280,720}, Output{1,640,480}})
            c.modes.push_back({int(c.modes.size()+1),size.width,size.height,15,60,{1,0x22,0x23,0x21},true});
        return c;
    }
};
} // namespace virtualcamera
