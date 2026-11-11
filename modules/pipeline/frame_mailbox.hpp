// modules/pipeline/frame_mailbox.hpp
#pragma once
#include "gpu_preprocessor.hpp"
class SharedFrameMailbox
{
public:
    int WriteSlot(bool detectorIdle, int slotCount)
    {
        if (detectorIdle) active = -1;
        if (slotCount <= 0) return -1;
        if (pending >= 0) return pending;
        if (active < 0) return 0;
        return slotCount > 1 ? 1 - active : -1;
    }
    void Commit(int slot, const LetterboxInfo& info) { pending = slot; lb = info; }
    int Take(bool detectorIdle, LetterboxInfo& info)
    {
        if (!detectorIdle) return -1;
        active = -1;
        if (pending < 0) return -1;
        active = pending; pending = -1; info = lb;
        return active;
    }
    bool HasPending() const noexcept { return pending >= 0; }
    void Reset() { active = pending = -1; lb = {}; }
private:
    int active = -1, pending = -1;
    LetterboxInfo lb{};
};
