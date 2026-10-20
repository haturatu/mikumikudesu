#include <chrono>
#include <iostream>
#include <mmd/animation.hpp>

int main() {
    mmd::PmxModel model;
    model.morphs.resize(4096);
    for (std::size_t i = 0; i + 1 < model.morphs.size(); ++i) {
        model.morphs[i].type = 0;
        model.morphs[i].offsets.push_back({.index = static_cast<std::int32_t>(i + 1), .scalar = 0.999F});
    }
    model.morphs.back().type = 1;
    std::vector<float> roots(model.morphs.size());
    roots.front() = 1;
    mmd::MorphWeightExpander cache(model);
    mmd::MorphExpansionResult cached;
    cache.expandInto(cached, roots);
    for (unsigned phase = 0; phase < 6; ++phase) {
        double checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned frame = 0; frame < 300; ++frame) {
            if (phase % 2 == 0) {
                const auto result = mmd::expandMorphWeights(model, roots);
                checksum += result.effectiveWeights.back();
            } else {
                cache.expandInto(cached, roots);
                checksum += cached.effectiveWeights.back();
            }
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << (phase % 2 ? "cached" : "oneoff") << " mean=" << ms / 300 << " checksum=" << checksum << '\n';
    }
}
