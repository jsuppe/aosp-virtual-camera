#include "FrameProfile.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>
using virtualcamera::FrameProfile;
int main() {
    char path[]="/tmp/vcam-profile-test-XXXXXX";
    assert(mkdtemp(path));
    FrameProfile off;
    { FrameProfile::Scope sample(off,1,120,1920,1080,4); }
    assert(off.write(path));
    assert(std::filesystem::is_empty(path));
    FrameProfile on; on.enable(77,0,123);
    for (int i=0;i<16385;i++) {
        FrameProfile::Scope sample(on,i,120,1920,1080,9);
        sample.mark(FrameProfile::Import);
        sample.source(i+1,200+i);
        if(i==1)sample.fail(FrameProfile::SourceFenceTimeout,FrameProfile::SourceFenceWait);
    }
    assert(on.write(path)); assert(on.write(path));
    int files=0,rows=0;
    for(const auto& entry:std::filesystem::directory_iterator(path)) {
        files++; std::ifstream input(entry.path());std::string line;
        std::getline(input,line);assert(line.find("dropped=1")!=std::string::npos);
        std::getline(input,line);
        while(std::getline(input,line)) {
            std::istringstream in(line);std::string field;std::vector<int64_t> values;
            while(std::getline(in,field,','))values.push_back(std::stoll(field));
            assert(values.size()==18);assert(values[8]==(rows==1?6:0));
            int64_t sum=0;for(size_t j=9;j<17;j++){assert(values[j]>=0);sum+=values[j];}
            assert(sum==values[17]);rows++;
        }
        std::filesystem::remove(entry.path());
    }
    assert(files==1 && rows==16384);std::filesystem::remove(path);
    puts("PASS: opt-in, bounded rows, failure reason, timing conservation, idempotent export");
}
