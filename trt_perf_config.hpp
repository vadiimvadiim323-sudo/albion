#pragma once
// Pure, platform-independent TensorRT perf-flag planning.
// No CUDA/Windows dependencies: covered by tests/trt_perf_config_test.cpp.
#include <array>
#include <cstddef>

// All perf flags are read once, before any graph is built.
struct TrtPerfConfig {
    bool gpuTiming=false, outputGraph=false, ioGraph=false, spinCompletion=false;
};
struct TrtGraphPlan {
    bool profile=false, captureOutput=false, ioGraph=false;
};
// Profiling is active only if requested AND every stage event exists.
// Graphs that hide stage boundaries are disabled while profiling.
inline TrtGraphPlan AimPlanTrtGraphs(const TrtPerfConfig& config, bool eventsReady) noexcept {
    TrtGraphPlan plan;
    plan.profile = config.gpuTiming && eventsReady;
    plan.captureOutput = config.outputGraph && !plan.profile;
    plan.ioGraph = config.ioGraph && !plan.profile;
    return plan;
}
// Creates all events or none; partially created events are destroyed.
template <class Event, std::size_t N, class Create, class Destroy>
bool AimCreateEventsAllOrNothing(std::array<Event, N>& events, Create create, Destroy destroy) {
    for (std::size_t i = 0; i < N; ++i) {
        if (!create(events[i])) {
            for (auto& event : events) {
                if (event) destroy(event);
                event = Event{};
            }
            return false;
        }
    }
    return true;
}
