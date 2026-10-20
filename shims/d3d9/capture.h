#pragma once

#include <string>

namespace shim {
// Thread-safe request; coalesces requests while a capture is pending/in progress.
bool request_gpu_dump();
// Render-thread access: the complete frame after a request was accepted.
bool gpu_dump_active();
const std::string& gpu_dump_directory();
void gpu_dump_error(const char* what);
// Controller feedback, implemented alongside XInput's owned gamepads.
void gpu_dump_finished(bool success);
}
