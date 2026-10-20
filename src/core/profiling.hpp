#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace dayo::core {

// Lightweight per-frame CPU/GPU profiler for the preview path. Sections
// record wall time on the calling thread; counters accumulate transfer
// sizes and draw statistics. GPU time is fed in from the backend (Vulkan
// timestamp queries) once per frame. All state fits in a single object
// owned by the application; it is not thread-safe by design.
enum class ProfileSection : std::uint8_t { animation, vertexConvert, upload, render };
inline constexpr std::size_t profileSectionCount = static_cast<std::size_t>(ProfileSection::render) + 1U;
static_assert(profileSectionCount == 4);

struct ProfileTotals {
    std::array<double, profileSectionCount> sectionSeconds{};
    std::array<std::uint64_t, 10> gpuPassNanoseconds{};
    std::uint64_t gpuSamples{};
    std::uint64_t uploadBytes{};
    std::uint64_t vertices{};
    std::uint64_t draws{};
    std::uint64_t gpuNanoseconds{};
    std::uint64_t frames{};
};

class FrameProfiler {
  public:
    class Section {
      public:
        Section(FrameProfiler* owner, ProfileSection section) noexcept
            : owner_(owner), section_(section), start_(std::chrono::steady_clock::now()) {}
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        ~Section() {
            finish();
        }
        void finish() noexcept {
            if (owner_ == nullptr)
                return;
            owner_->addSection(section_, std::chrono::steady_clock::now() - start_);
            owner_ = nullptr;
        }

      private:
        FrameProfiler* owner_;
        ProfileSection section_;
        std::chrono::steady_clock::time_point start_;
    };

    [[nodiscard]] Section measure(ProfileSection section) noexcept {
        return Section(this, section);
    }
    void beginFrame(double periodSeconds = 0.0) {
        if (std::isfinite(periodSeconds) && periodSeconds > 0.0) {
            periods_[periodCursor_] = periodSeconds;
            periodCursor_ = (periodCursor_ + 1U) % periods_.size();
            periodCount_ = std::min(periodCount_ + 1U, periods_.size());
        }
        current_ = ProfileTotals{};
        frameActive_ = true;
    }
    void endFrame() {
        if (!frameActive_)
            return;
        for (std::size_t i = 0; i < totals_.sectionSeconds.size(); ++i)
            totals_.sectionSeconds[i] += current_.sectionSeconds[i];
        for (std::size_t i = 0; i < totals_.gpuPassNanoseconds.size(); ++i)
            totals_.gpuPassNanoseconds[i] += current_.gpuPassNanoseconds[i];
        totals_.gpuSamples += current_.gpuSamples;
        totals_.uploadBytes += current_.uploadBytes;
        totals_.vertices += current_.vertices;
        totals_.draws += current_.draws;
        totals_.gpuNanoseconds += current_.gpuNanoseconds;
        ++totals_.frames;
        current_ = ProfileTotals{};
        frameActive_ = false;
    }
    void addSection(ProfileSection section, std::chrono::steady_clock::duration elapsed) noexcept {
        if (!frameActive_)
            return;
        current_.sectionSeconds[static_cast<std::size_t>(section)] += std::chrono::duration<double>(elapsed).count();
    }
    void addUploadBytes(std::uint64_t bytes) noexcept {
        if (!frameActive_)
            return;
        current_.uploadBytes += bytes;
    }
    void addDrawStats(std::uint64_t vertices, std::uint64_t draws) noexcept {
        if (!frameActive_)
            return;
        current_.vertices += vertices;
        current_.draws += draws;
    }
    void setGpuNanoseconds(std::uint64_t nanoseconds) noexcept {
        if (!frameActive_)
            return;
        current_.gpuNanoseconds = nanoseconds;
    }
    void setGpuPassNanoseconds(const std::array<std::uint64_t, 10>& timings) noexcept {
        if (!frameActive_ || current_.gpuNanoseconds == 0)
            return;
        current_.gpuPassNanoseconds = timings;
        current_.gpuSamples = 1;
    }
    // Exact nearest-rank percentile over the last 4096 observed frame periods.
    // Storage is bounded and sampling does not allocate or synchronize the GPU.
    [[nodiscard]] double framePeriodPercentile(double fraction) const {
        if (periodCount_ == 0)
            return 0.0;
        auto sorted = periods_;
        const auto index = static_cast<std::size_t>(std::ceil(
            std::clamp(std::isfinite(fraction) ? fraction : 0.95, 0.0, 1.0) * static_cast<double>(periodCount_)));
        const auto selected = index == 0 ? 0 : index - 1U;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(selected),
                         sorted.begin() + static_cast<std::ptrdiff_t>(periodCount_));
        return sorted[selected];
    }
    [[nodiscard]] std::string gpuPassReport() const;
    [[nodiscard]] const ProfileTotals& totals() const noexcept {
        return totals_;
    }
    // One-line human-readable summary averaged over recorded frames.
    [[nodiscard]] std::string report() const;
    void reset() noexcept {
        totals_ = ProfileTotals{};
        current_ = ProfileTotals{};
        periodCount_ = periodCursor_ = 0;
        frameActive_ = false;
    }

  private:
    std::array<double, 4096> periods_{};
    std::size_t periodCount_{};
    std::size_t periodCursor_{};
    ProfileTotals totals_;
    ProfileTotals current_;
    bool frameActive_{};
};

} // namespace dayo::core
