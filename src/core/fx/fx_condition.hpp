#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::core::fx {

// Backend-neutral condition layer: string conditions compile to an event
// mask plus an opaque predicate. The scheduler only matches event masks;
// predicate evaluation stays with the runtime.
enum class FxEvent : std::uint32_t {
    none = 0U,
    load = 1U << 0U,
    start = 1U << 1U,
    resize = 1U << 2U,
    frame = 1U << 3U,
    modelChanged = 1U << 4U,
    materialChanged = 1U << 5U,
};

using FxEventMask = std::uint32_t;

inline constexpr FxEventMask kFxEventNone = 0U;
inline constexpr FxEventMask kFxEventLoad = 1U << 0U;
inline constexpr FxEventMask kFxEventStart = 1U << 1U;
inline constexpr FxEventMask kFxEventResize = 1U << 2U;
inline constexpr FxEventMask kFxEventFrame = 1U << 3U;
inline constexpr FxEventMask kFxEventModelChanged = 1U << 4U;
inline constexpr FxEventMask kFxEventMaterialChanged = 1U << 5U;
inline constexpr FxEventMask kFxEventAll =
    kFxEventLoad | kFxEventStart | kFxEventResize | kFxEventFrame | kFxEventModelChanged | kFxEventMaterialChanged;

[[nodiscard]] constexpr FxEventMask toMask(FxEvent event) noexcept {
    return static_cast<FxEventMask>(event);
}

[[nodiscard]] const char* toString(FxEvent event) noexcept;
[[nodiscard]] FxEventMask fxEventFromName(std::string_view name) noexcept;
[[nodiscard]] std::string toStringMask(FxEventMask mask);

// Compiled condition: event mask to schedule on plus the raw predicate
// text after `if`/`when`/`:` (empty means unconditional for those events).
struct FxConditionProgram {
    FxEventMask events{kFxEventNone};
    std::string predicate;
    std::vector<std::string> eventNames;
};

// Accepted forms (case-insensitive, `on`/`when` prefix optional):
//   "on load" / "load, frame" / "load+frame if time > 0" / "when resize: w>0".
// Unknown event names throw std::runtime_error.
[[nodiscard]] FxConditionProgram compileFxCondition(std::string_view source);

// Empty event mask means unconditional: matches every active mask.
[[nodiscard]] bool fxConditionMatches(const FxConditionProgram& program, FxEventMask active) noexcept;

struct FxScheduledPass {
    FxConditionProgram condition;
    int passIndex{-1};
    std::string passName;
};

class FxConditionScheduler {
  public:
    void add(FxConditionProgram condition, int passIndex, std::string passName = {});
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return passes_.size();
    }
    [[nodiscard]] const std::vector<FxScheduledPass>& passes() const noexcept {
        return passes_;
    }
    // Returns pass indices whose condition matches `active`.
    [[nodiscard]] std::vector<int> activePasses(FxEventMask active) const;

  private:
    std::vector<FxScheduledPass> passes_;
};

} // namespace dayo::core::fx
