#include "core/profiling.hpp"

#include <cstdio>

namespace dayo::core {

std::string FrameProfiler::report() const {
    if (totals_.frames == 0)
        return "profile: no frames recorded";
    const auto frames = static_cast<double>(totals_.frames);
    const auto average = [&](std::size_t section) { return totals_.sectionSeconds[section] / frames * 1000.0; };
    const auto uploadMiBs = static_cast<double>(totals_.uploadBytes) / frames / 1048576.0;
    const auto gpuMs = static_cast<double>(totals_.gpuNanoseconds) / frames / 1000000.0;
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "profile: %llu frames animation=%.2fms convert=%.2fms upload=%.2fms render=%.2fms "
                  "gpu=%.2fms up=%.2fMiB/f verts=%llu draws=%llu p95=%.2fms",
                  static_cast<unsigned long long>(totals_.frames), average(0), average(1), average(2), average(3),
                  gpuMs, uploadMiBs, static_cast<unsigned long long>(totals_.vertices / totals_.frames),
                  static_cast<unsigned long long>(totals_.draws / totals_.frames),
                  framePeriodPercentile(0.95) * 1000.0);
    return buffer;
}

std::string FrameProfiler::gpuPassReport() const {
    if (totals_.gpuSamples == 0)
        return "gpu passes: unavailable";
    const auto milliseconds = [&](std::size_t i) {
        return static_cast<double>(totals_.gpuPassNanoseconds[i]) / static_cast<double>(totals_.gpuSamples) / 1e6;
    };
    char buffer[384];
    std::snprintf(buffer, sizeof(buffer),
                  "gpu passes: samples=%llu shadow=%.3fms depth/ao=%.3fms materials=%.3fms sss=%.3fms "
                  "coc=%.3fms downsample=%.3fms blur=%.3fms composite=%.3fms bloom=%.3fms tonemap=%.3fms",
                  static_cast<unsigned long long>(totals_.gpuSamples), milliseconds(0), milliseconds(1),
                  milliseconds(2), milliseconds(3), milliseconds(4), milliseconds(5), milliseconds(6), milliseconds(7),
                  milliseconds(8), milliseconds(9));
    return buffer;
}

} // namespace dayo::core
