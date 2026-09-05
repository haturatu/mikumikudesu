#include "core/upload_ring.hpp"

#include <iostream>

int main() {
    using dayo::core::UploadRing;
    UploadRing ring(64, 16);
    const auto first = ring.tryAllocate(12, 1);
    if (!first || first->offset != 0 || ring.used() != 12)
        return 1;
    const auto second = ring.tryAllocate(8, 2);
    if (!second || second->offset != 16)
        return 2;
    if (ring.tryAllocate(40, 3))
        return 3;
    ring.reclaim(1);
    if (ring.used() != 12)
        return 4;
    const auto third = ring.tryAllocate(24, 4);
    if (!third || third->offset != 32)
        return 5;
    ring.reclaim(2);
    const auto tail = ring.tryAllocate(8, 5);
    if (!tail || tail->offset != 0)
        return 6;
    ring.reclaim(5);
    if (ring.used() != 0 || ring.available() != ring.capacity())
        return 7;
    std::cout << "upload ring tests passed\n";
    return 0;
}
