// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/ZzzVertexChecks.h"

#include <array>
#include <cassert>
#include <vector>

struct Buffer {
    int id;
    int references = 0;

    void Release()
    {
        assert(references > 0);
        --references;
    }
};

int main()
{
    Buffer first{1}, second{2}, last{3};
    const auto check = [&](const std::array<Buffer*, 4>& slots, int match,
        std::size_t expectedIndex, const std::vector<int>& expectedProbes) {
        int reads = 0;
        std::vector<int> probes;
        const auto index = ZzzVertexChecks::FirstMatch<Buffer>(slots.size(),
            [&](Buffer** bindings) {
                ++reads;
                for (std::size_t i = 0; i < slots.size(); ++i) {
                    bindings[i] = slots[i];
                    if (bindings[i])
                        ++bindings[i]->references;
                }
            }, [&](Buffer* buffer) {
                assert(buffer->references > 0);
                probes.push_back(buffer->id);
                return buffer->id == match;
            });

        assert(index == expectedIndex);
        assert(reads == 1);
        assert(probes == expectedProbes);
        assert(first.references == 0 && second.references == 0 && last.references == 0);
    };

    check({&first, nullptr, &second, &last}, 0, 4, {1, 2, 3});
    check({&first, &second, &last, &first}, 1, 0, {1});
    check({nullptr, &first, &second, &last}, 2, 2, {1, 2});
    check({&first, &first, nullptr, &last}, 3, 3, {1, 1, 3});
}
