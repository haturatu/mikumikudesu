#include "core/image.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: texture-bench DDS...\n";
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        try {
            auto image = dayo::core::loadTextureImage(argv[i]);
            std::uint64_t hash = 14695981039346656037ULL;
            std::size_t bytes = 0;
            for (const auto& sub : image.subresources) {
                for (auto x : sub.pixels) {
                    hash ^= x;
                    hash *= 1099511628211ULL;
                }
                bytes += sub.pixels.size();
            }
            std::vector<double> times;
            for (int repeat = 0; repeat < 11; ++repeat) {
                auto start = std::chrono::steady_clock::now();
                auto sample = dayo::core::loadTextureImage(argv[i]);
                auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (repeat)
                    times.push_back(ms);
                if (sample.format != image.format)
                    return 1;
            }
            std::sort(times.begin(), times.end());
            std::cout << argv[i] << "\t" << image.format << "\t" << bytes << "\t" << hash << "\t"
                      << (times[4] + times[5]) * 0.5 << "\n";
        } catch (const std::exception& e) {
            std::cerr << argv[i] << ": " << e.what() << "\n";
            return 1;
        }
    }
}
