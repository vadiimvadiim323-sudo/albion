// modules/core/timing.hpp
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
struct DetectionTiming
{
    std::uint64_t frameId = 0;
    double capturedMs = 0.0, submittedMs = 0.0, completedMs = 0.0;
    double inferenceMs = 0.0, decodeMs = 0.0;
    double workerStartedMs=0.0, presentedMs=0.0;
    double acquiredMs=0;
    std::uint32_t accumulatedFrames=0;
    float roiX=0,roiY=0,roiWidth=0,roiHeight=0,letterboxRatio=1,padX=0,padY=0;
    int netSide=0;
};
// Set true ONLY for a model whose export contract guarantees built-in NMS.
// Layout heuristics alone are not a sufficient guarantee.
inline constexpr bool kModelOutputHasVerifiedNms = false;
// Bounded, allocation-free capture-to-result history. Read under mResMx.
struct LatencyWindow {
    std::array<double, 256> values{};
    size_t count = 0, next = 0;
    void Add(double ms) noexcept {
        if (!std::isfinite(ms) || ms < 0.0) return;
        values[next] = ms; next = (next + 1) % values.size();
        count = std::min(count + 1, values.size());
    }
    double Quantile(double q) const {
        if (!count) return -1.0;
        auto sorted = values;
        std::sort(sorted.begin(), sorted.begin() + count);
        const size_t rank = size_t(std::ceil(std::clamp(q, 0.0, 1.0) * double(count)));
        return sorted[rank ? rank - 1 : 0]; // nearest-rank percentile
    }
};
