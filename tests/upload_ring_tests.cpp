#include "core/upload_ring.hpp"

#include <iostream>
#include <stdexcept>

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

    UploadRing nonPowerOfTwo(60, 8);
    const auto fullNonPowerOfTwo = nonPowerOfTwo.tryAllocate(60, 1);
    if (!fullNonPowerOfTwo || fullNonPowerOfTwo->offset != 0 || nonPowerOfTwo.used() != 60)
        return 8;

    bool invalidAlignmentRejected = false;
    try {
        static_cast<void>(UploadRing(64, 3));
    } catch (const std::invalid_argument&) {
        invalidAlignmentRejected = true;
    }
    if (!invalidAlignmentRejected)
        return 9;
    if (nonPowerOfTwo.tryAllocate(1, 2, 3))
        return 10;

    UploadRing exactEnd(64, 16);
    const auto exact = exactEnd.tryAllocate(64, 1);
    if (!exact || exact->offset != 0 || exactEnd.tryAllocate(1, 2))
        return 11;
    exactEnd.reclaim(1);
    if (exactEnd.used() != 0)
        return 12;

    UploadRing paddedWrap(64, 16);
    const auto paddingFirst = paddedWrap.tryAllocate(20, 1);
    const auto paddingSecond = paddedWrap.tryAllocate(8, 2);
    paddedWrap.reclaim(1);
    const auto paddingThird = paddedWrap.tryAllocate(8, 3, 8);
    const auto wrapped = paddedWrap.tryAllocate(17, 4);
    if (!paddingFirst || !paddingSecond || !paddingThird || !wrapped || wrapped->offset != 0 || paddedWrap.used() != 61)
        return 13;
    paddedWrap.reclaim(4);
    if (paddedWrap.used() != 0)
        return 14;

    UploadRing lifecycle(48, 8);
    if (!lifecycle.tryAllocate(32, 10) || !lifecycle.tryAllocate(16, 11) || lifecycle.tryAllocate(1, 12))
        return 15;
    lifecycle.reclaim(10);
    if (!lifecycle.tryAllocate(16, 12))
        return 16;
    lifecycle.reclaim(11);
    if (!lifecycle.tryAllocate(24, 13))
        return 17;
    lifecycle.reclaim(13);
    if (lifecycle.used() != 0)
        return 18;

    UploadRing ordered(32);
    if (!ordered.tryAllocate(8, 100) || ordered.tryAllocate(8, 20) || ordered.used() != 8)
        return 19;
    ordered.reclaim(100);
    if (!ordered.tryAllocate(8, 20))
        return 20;

    std::cout << "upload ring tests passed\n";
    return 0;
}
