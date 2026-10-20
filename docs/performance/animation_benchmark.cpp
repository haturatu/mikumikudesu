#include <algorithm>
#include <chrono>
#include <iostream>
#include <mmd/animation.hpp>
#include <mmd/pmx.hpp>
#include <mmd/vmd.hpp>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    auto model = mmd::pmx::load(argv[1]);
    auto motion = mmd::vmd::load(argv[2]);
    const auto norm = mmd::previewNormalization(model);
    for (int repeat = 0; repeat < 6; ++repeat) {
        auto mode = repeat % 2 ? mmd::VertexOutput::omitUnchangedGpu : mmd::VertexOutput::full;
        mmd::MmdAnimator animator(model);
        animator.setMotion(&motion);
        std::vector<double> timings;
        double checksum = 0;
        std::size_t vertices = 0;
        for (int i = 0; i < 310; ++i) {
            auto start = std::chrono::steady_clock::now();
            auto frame = animator.evaluate(static_cast<float>(i) * 0.5F, 1.0F / 60.0F, true, {}, mode);
            mmd::normalizeForPreview(frame.vertices, norm);
            auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (i >= 10)
                timings.push_back(ms);
            vertices = frame.vertices.size();
            for (const auto& bone : frame.bones)
                checksum += bone.translation[0] + bone.translation[1] + bone.translation[2];
        }
        double sum = 0;
        for (auto value : timings)
            sum += value;
        std::sort(timings.begin(), timings.end());
        std::cout << "mode=" << (repeat % 2 ? "omit" : "full") << " mean=" << sum / timings.size()
                  << " p95=" << timings[284] << " vertices=" << vertices
                  << " bytes=" << vertices * sizeof(mmd::PmxVertex) << " checksum=" << checksum << '\n';
    }
}
