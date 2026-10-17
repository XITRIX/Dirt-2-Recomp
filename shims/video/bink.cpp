// binkw32: video playback. Until the FFmpeg-based decoder lands, BinkOpen
// reports the file as unavailable and the game skips its videos; the rest of
// the API answers consistently.
#include "../shim.h"

// Decorated stdcall names (_Name@bytes).
#define BINK(name, bytes) SHIM_DEFINE(imp_binkw32__##name##_##bytes, binkw32, "_" #name "@" #bytes, 0, bytes)

BINK(BinkOpen, 8) {  // (const char* name, flags) -> HBINK
    rt::log("BinkOpen(%s): video playback not implemented yet, skipping", a.str(0) ? a.str(0) : "");
    return 0;
}
BINK(BinkClose, 4) { return 0; }
BINK(BinkDoFrame, 4) { return 0; }
BINK(BinkNextFrame, 4) { return 0; }
BINK(BinkWait, 4) { return 0; }
BINK(BinkShouldSkip, 4) { return 0; }
BINK(BinkPause, 8) { return 0; }
BINK(BinkGoto, 12) { return 0; }
BINK(BinkSetVolume, 12) { return 0; }
BINK(BinkSetMixBins, 16) { return 0; }
BINK(BinkSetMemory, 8) { return 0; }
BINK(BinkSetSoundTrack, 8) { return 0; }
BINK(BinkSetSoundSystem, 8) { return 1; }
BINK(BinkOpenDirectSound, 4) { return 0; }  // only ever passed to BinkSetSoundSystem
BINK(BinkRegisterFrameBuffers, 8) { return 0; }
BINK(BinkGetFrameBuffersInfo, 8) {
    if (a[1]) std::memset(rt::host_ptr(a[1]), 0, 64);
    return 0;
}
