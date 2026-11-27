// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>

namespace ZzzVertexChecks {
constexpr std::size_t maxBatchSize = 4;

// The getter gives us one owning reference for each non-null binding. Finish
// probing and drop those references before any mod commands can change state.
template<class Buffer, class Read, class Probe>
std::size_t FirstMatch(std::size_t count, Read read, Probe probe)
{
    struct Bindings {
        std::array<Buffer*, maxBatchSize> slots{};

        void release(std::size_t slot)
        {
            if (slots[slot]) {
                slots[slot]->Release();
                slots[slot] = nullptr;
            }
        }

        ~Bindings()
        {
            for (std::size_t i = 0; i < slots.size(); ++i)
                release(i);
        }
    } bindings;

    read(bindings.slots.data());
    for (std::size_t i = 0; i < count; ++i) {
        const bool matched = bindings.slots[i] && probe(bindings.slots[i]);
        bindings.release(i);
        if (matched)
            return i;
    }
    return count;
}
} // namespace ZzzVertexChecks
