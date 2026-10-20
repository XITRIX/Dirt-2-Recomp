#pragma once
#include <cstdint>

namespace rt::sampling {
struct CaptureResult {
    uint32_t pause = 0, read = 0, resume = 0;
    bool suspended = false;
};
// Read is a nonthrowing kernel call. Always resume after a successful pause,
// including failed reads. Do not allocate, log or take another lock in here.
template<class Pause, class Read, class Resume>
CaptureResult capture(Pause pause, Read read, Resume resume) {
    CaptureResult result;
    result.pause = pause();
    if (result.pause) return result;
    result.suspended = true;
    result.read = read();
    result.resume = resume();
    return result;
}
}
