#include "core/scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: dayo_model_load_bench MODEL MOTION\n";
        return 1;
    }
    try {
        std::array<double, 3> models{}, motions{};
        for (unsigned i = 0; i < 4; ++i) {
            dayo::core::Scene scene;
            const auto start = std::chrono::steady_clock::now();
            const auto id = scene.addModel(argv[1]);
            const auto loaded = std::chrono::steady_clock::now();
            scene.selectModel(id);
            scene.importMotion(dayo::core::loadVmd(argv[2]));
            const auto end = std::chrono::steady_clock::now();
            if (i != 0) {
                models[i - 1] = std::chrono::duration<double, std::milli>(loaded - start).count();
                motions[i - 1] = std::chrono::duration<double, std::milli>(end - loaded).count();
            }
        }
        std::sort(models.begin(), models.end());
        std::sort(motions.begin(), motions.end());
        std::cout << "model_ms=" << models[1] << " motion_ms=" << motions[1] << '\n';
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
