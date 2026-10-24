#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace virtualcamera {
// Opt-in CPU elapsed times. GPU submission time is NOT GPU execution time.
// No files/logs are written on the capture path; storage is bounded.
class FrameProfile {
public:
    enum Phase { Import, DestinationWait, Pace, SourceWait, SourceFenceWait,
                 Output, Callback, Cleanup, PhaseCount };
    enum Failure { None, FenceDup, InvalidImport, DestinationTimeout, Cancelled,
                   NoSource, SourceFenceTimeout, StaleSource, NoFilledOutput };
    struct Row {
        int64_t start=0, total=0, token=0, producer=0;
        int64_t acquired=0, deadline=0, fenceSignal=0, fenceObserved=0;
        int fenceStatus=-2;
        uint64_t sequence=0;
        int frame=0, fps=0, width=0, height=0, failure=None;
        std::array<int64_t, PhaseCount> ns{};
    };
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void enable(int run, int slot, int64_t session) {
        mRun=run; mSlot=slot; mSession=session;
        if (run>0) mRows.reserve(kLimit);
    }
    bool enabled() const { return mRun>0; }
    class Scope {
    public:
        Scope(FrameProfile& owner,int frame,int fps,int width,int height,int64_t token)
            : mOwner(owner.enabled()?&owner:nullptr) {
            if (!mOwner) return;
            mRow.frame=frame; mRow.fps=fps; mRow.width=width; mRow.height=height;
            mRow.token=token; mLast=mRow.start=now();
        }
        ~Scope() { finish(); }
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
        void mark(Phase phase) {
            if (!mOwner) return;
            int64_t t=now(); mRow.ns[phase]+=t-mLast; mLast=t;
        }
        void fail(Failure failure,Phase phase) {
            if (!mOwner) return;
            mark(phase); mRow.failure=failure;
        }
        void source(uint64_t sequence,int64_t timestamp) {
            if (mOwner) { mRow.sequence=sequence; mRow.producer=timestamp; mRow.acquired=now(); }
        }
        void fence(int64_t deadline,int64_t signal,int status) {
            if(mOwner) { mRow.deadline=deadline; mRow.fenceSignal=signal;
                mRow.fenceStatus=status; mRow.fenceObserved=now(); }
        }
        void finish() {
            if (!mOwner) return;
            mark(Cleanup); mRow.total=mLast-mRow.start;
            if (mOwner->mRows.size()<kLimit) mOwner->mRows.push_back(mRow);
            else ++mOwner->mDropped;
            mOwner=nullptr;
        }
    private:
        FrameProfile* mOwner;
        Row mRow;
        int64_t mLast=0;
    };
    bool write(const char* directory) {
        if (!enabled() || mWritten) return true;
        char path[512];
        snprintf(path,sizeof(path),"%s/hal-%d-%d-%d-%lld.csv",directory,mRun,
                int(getpid()),mSlot,(long long)mSession);
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0640);
        if (fd<0) return false;
        FILE* f=fdopen(fd,"w");
        if (!f) { close(fd); return false; }
        fprintf(f,"# schema=2 run=%d session=%lld dropped=%zu\n",mRun,
                (long long)mSession,mDropped);
        fprintf(f,"start_ns,frame,fps,width,height,token,source_sequence,producer_ns,failure,import_ns,destination_wait_ns,pace_ns,source_wait_ns,source_fence_wait_ns,output_ns,callback_ns,cleanup_ns,total_ns,source_acquired_ns,deadline_ns,fence_signal_ns,fence_observed_ns,fence_status\n");
        for (const auto& r:mRows) {
            fprintf(f,"%lld,%d,%d,%d,%d,%lld,%llu,%lld,%d",(long long)r.start,
                    r.frame,r.fps,r.width,r.height,(long long)r.token,
                    (unsigned long long)r.sequence,(long long)r.producer,r.failure);
            for (auto ns:r.ns) fprintf(f,",%lld",(long long)ns);
            fprintf(f,",%lld,%lld,%lld,%lld,%lld,%d\n",(long long)r.total,
                    (long long)r.acquired,(long long)r.deadline,(long long)r.fenceSignal,
                    (long long)r.fenceObserved,r.fenceStatus);
        }
        bool good=!ferror(f); if (fclose(f)!=0) good=false;
        mWritten=good;
        return good;
    }
private:
    static constexpr size_t kLimit=16384;
    int mRun=0,mSlot=0;
    int64_t mSession=0;
    size_t mDropped=0;
    bool mWritten=false;
    std::vector<Row> mRows;
};
}
