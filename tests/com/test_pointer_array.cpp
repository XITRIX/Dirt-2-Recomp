#include "shims/com/pointer_array.h"
#include <cassert>
#include <array>
#include <utility>
int main() {
    PointerArray<int*> a;
    std::array<int, 128> objects{};
    for (unsigned count : {0u, 1u, 16u, 17u, 128u, 3u, 0u}) {
        auto* p = a.resize(count);
        assert((p != nullptr) == (count != 0));
        for (unsigned i = 0; i < count; ++i) p[i] = &objects[i];
        for (unsigned i = 0; i < count; ++i) assert(a[i] == &objects[i]);
        auto moved = std::move(a);
        for (unsigned i = 0; i < count; ++i) assert(moved[i] == &objects[i]);
        a = std::move(moved);
        // Getter calls must clear prior input values on both stack and heap
        // paths, including a large getter followed by a small one.
        p = a.resize(count, true);
        for (unsigned i = 0; i < count; ++i) assert(p[i] == nullptr);
    }
}
