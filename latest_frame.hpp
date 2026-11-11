#pragma once
#include <mutex>
#include <optional>
#include <cstdint>
#include <utility>

// Owns only the latest unconsumed frame. A taken frame belongs to its consumer;
// publishing never mutates that frame. No raw pointers into a recycled pool.
template<class Frame> class LatestFrameMailbox {
public:
    struct Counts {std::uint64_t published=0,taken=0,replaced=0,discarded=0;};
    bool Publish(Frame frame) {
        std::optional<Frame> retired;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if(mClosed)return false;
            if(mLatest){retired=std::move(mLatest);++mCounts.replaced;}
            mLatest=std::move(frame);++mCounts.published;
        }
        // Release a replaced GPU frame outside the mailbox mutex.
        return true;
    }
    std::optional<Frame> Take() {
        std::lock_guard<std::mutex> lock(mMutex);
        if(!mLatest)return {};
        auto frame=std::move(mLatest);mLatest.reset();++mCounts.taken;return frame;
    }
    void Close() {
        std::optional<Frame> retired;
        {std::lock_guard<std::mutex> lock(mMutex);mClosed=true;if(mLatest)++mCounts.discarded;retired=std::move(mLatest);mLatest.reset();}
    }
    Counts Stats() const {std::lock_guard<std::mutex> lock(mMutex);return mCounts;}
private:
    mutable std::mutex mMutex;
    std::optional<Frame> mLatest;
    Counts mCounts{};bool mClosed=false;
};
