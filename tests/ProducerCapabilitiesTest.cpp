#include "ProducerCapabilities.h"
#include <cassert>
#include <cstdio>
using namespace virtualcamera;
int main() {
    ProducerCapabilities c;c.registration=42;
    c.modes={{1,1920,1080,15,60,{1,34,35},false},{2,3840,2160,15,30,{1,34,35,33},true}};
    assert(c.valid());
    assert(c.select({{35,1920,1080}},60,60)->id==1);
    assert(c.select({{35,3840,2160}},30,30)->id==2);
    assert(!c.select({{35,3840,2160}},60,60));
    assert(!c.select({{35,3840,2160}},30,120));
    assert(!c.select({{35,1280,720}},30,30)); // smaller is not implicitly advertised
    assert(!c.select({{33,1920,1080}},30,30)); // no JPEG at this output mode
    assert(c.select({{35,3840,2160},{34,1920,1080}},30,30)->id==2);
    assert(!c.select({{35,3840,2160},{34,1920,1080}},60,60));
    auto disabled=c;disabled.modes[1].downscale=false;
    assert(!disabled.select({{35,3840,2160},{34,1920,1080}},30,30));
    assert(!c.select({{33,3840,2160},{33,3840,2160}},30,30));
    assert(!c.select({{35,1920,1080},{35,1920,1080},{35,1920,1080}},30,30));
    assert(!c.select({{35,1920,1080}},0,60));
    assert(!c.select({{35,1920,1080}},60,30));
    assert(!c.select({{35,1920,1080}},20,25)); // rates not advertised by this declaration
    assert(c.select({{35,1920,1080}},30,30)); // default cadence advertised inside 15-60
    for(int field=0;field<12;field++) {
        auto bad=c;
        switch(field) {
            case 0:bad.registration=0;break;case 1:bad.facing=3;break;case 2:bad.orientation=1;break;
            case 3:bad.maxStreams=4;break;case 4:bad.modes[0].width=8194;break;
            case 5:bad.modes[0].height=1079;break;case 6:bad.modes[0].minFps=0;break;
            case 7:bad.modes[0].maxFps=121;break;case 8:bad.modes[0].formats={99};break;
            case 9:bad.modes[0].formats={1,1};break;case 10:bad.modes[1].id=1;break;
            case 11:bad.modes[0].width=8192;bad.modes[0].height=8192;break;
        }
        assert(!bad.valid());assert(!bad.select({{35,1920,1080}},30,30));
    }
    auto portrait=c;portrait.modes={{3,1080,1920,15,30,{35},false}};
    assert(portrait.valid());assert(portrait.select({{35,1080,1920}},30,30));
    assert(!portrait.select({{35,1920,1080}},30,30));
    auto a=c;a.registration=43;a.modes={{1,1920,1080,15,30,{35},false}};
    assert(!a.advertised({35,3840,2160}));assert(c.advertised({35,3840,2160}));
    auto disjoint=c;
    disjoint.modes={{1,1920,1080,60,60,{35},false},{2,3840,2160,30,30,{35},false}};
    assert(disjoint.valid());
    assert(disjoint.defaultRange()==std::make_pair(30,60));
    assert(disjoint.fpsRanges().count(disjoint.defaultRange()));
    assert(disjoint.select({{35,1920,1080}},30,60)->id==1);
    assert(disjoint.select({{35,3840,2160}},30,60)->id==2);
    assert(!disjoint.select({{35,3840,2160}},60,60));
    assert(!disjoint.select({{35,1920,1080}},30,30));
    std::reverse(disjoint.modes.begin(),disjoint.modes.end());
    assert(disjoint.defaultRange()==std::make_pair(30,60));
    assert(c.defaultRange()==std::make_pair(30,30));
    // Regression: raising a mode ceiling60->120 does not implicitly advertise
    // [60,60]. Runtime selectors must retain an explicit60 endpoint mode.
    auto high=c;
    high.modes={{1,640,480,15,120,{1,34,35},false},
                {2,1920,1080,15,120,{1,34,35},false},
                {3,1280,720,15,30,{1,34,35},false}};
    assert(high.select({{1,1920,1080}},120,120));
    assert(!high.select({{1,1920,1080}},60,60));
    high.modes.push_back({4,640,480,15,60,{1,34,35},false});
    high.modes.push_back({5,1920,1080,15,60,{1,34,35},false});
    for (auto size : {ProducerCapabilities::Output{1,640,480}, {1,1920,1080}})
        for (int rate : {30,60,120}) assert(high.select({size},rate,rate));
    assert(!high.select({{1,1280,720}},60,60));
    std::puts("PASS: explicit profiles, adaptive disjoint rates, FPS/size/format/combination isolation and malformed descriptor rejection");
}
