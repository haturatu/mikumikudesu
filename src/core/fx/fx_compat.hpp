#pragma once

namespace dayo::core::fx {

// MikuMikuDayo 1.30 compatibility baseline vs native extensions.
// GPU code paths must not depend on this; it only switches CPU-side
// semantic interpretation (expression quirks, size rules).
enum class FxCompatibilityProfile {
    upstream130,
    nativeExtended,
};

[[nodiscard]] inline const char* toString(FxCompatibilityProfile profile) noexcept {
    switch (profile) {
    case FxCompatibilityProfile::upstream130:
        return "upstream130";
    case FxCompatibilityProfile::nativeExtended:
        return "nativeExtended";
    }
    return "unknown";
}

} // namespace dayo::core::fx
