#pragma once
// FIX-V11-03: No C++ exception may escape a std::thread entry point. Failure
// reporting itself is best effort (including low-memory failure paths).
#include <exception>
#include <utility>
namespace albion {
template<class Work, class Failure, class Exit>
void GuardWorker(Work&& work, Failure&& failure, Exit&& exit) noexcept {
    try { work(); }
    catch (const std::exception& e) { try { failure(e.what()); } catch (...) {} }
    catch (...) { try { failure("Unknown worker exception"); } catch (...) {} }
    try { exit(); } catch (...) {}
}
}
