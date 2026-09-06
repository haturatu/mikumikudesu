#pragma once

#include "fx/fx_catalog.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace dayo::fx {

// Frame stage ordering for the reference scheduler:
//   deform (all models) -> renderer -> postPre -> tonemap -> postPost -> present
// Deform and present are synthetic stages always present; the rest come
// from the catalog. Controller-off entries are skipped.
enum class FrameStage : std::uint8_t {
    deform,
    renderer,
    postPre,
    tonemap,
    postPost,
    present,
};

[[nodiscard]] const char* toString(FrameStage stage) noexcept;

struct ScheduledFx {
    std::string name;
    FrameStage stage{FrameStage::renderer};
    int order{};
};

class FrameEffectScheduler {
  public:
    void setControllerEnabled(std::string name, bool enabled);
    [[nodiscard]] bool isEnabled(const std::string& name) const noexcept;
    // Build the ordered per-frame list. rendererName selects the active
    // renderer entry; empty selects the first renderer entry.
    [[nodiscard]] std::vector<ScheduledFx> schedule(const EffectCatalog& catalog,
                                                    const std::string& rendererName = {}) const;

  private:
    static FrameStage stageFor(const FxCatalogEntry& entry) noexcept;

    std::unordered_map<std::string, bool> controllerOff_;
};

} // namespace dayo::fx
