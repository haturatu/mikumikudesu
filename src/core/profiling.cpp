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
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "profile: %llu frames animation=%.2fms convert=%.2fms upload=%.2fms render=%.2fms "
                  "gpu=%.2fms up=%.2fMiB/f verts=%llu draws=%llu",
                  static_cast<unsigned long long>(totals_.frames), average(0), average(1), average(2), average(3),
                  gpuMs, uploadMiBs, static_cast<unsigned long long>(totals_.vertices / totals_.frames),
                  static_cast<unsigned long long>(totals_.draws / totals_.frames));
    return buffer;
}

} // namespace dayo::core
