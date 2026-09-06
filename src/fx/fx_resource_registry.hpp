#pragma once

#include "fx/fx_compiler.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dayo::fx {

// Central registry for live programs and frame contexts. The graphics
// executor and the application resolve Dayo:: scene values through here
// (or directly through FxFrameContext) instead of reaching into Scene
// mid-frame. Thread-safe; frame swap is atomic shared_ptr exchange.
class FxResourceRegistry {
  public:
    void registerProgram(const std::string& name, std::shared_ptr<const FxProgram> program);
    [[nodiscard]] std::shared_ptr<const FxProgram> findProgram(const std::string& name) const;
    void unregisterProgram(const std::string& name);
    void storeFrameContext(FxFrameContext context);
    [[nodiscard]] FxFrameContext loadFrameContext() const;
    void clear() noexcept;
    [[nodiscard]] std::size_t programCount() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const FxProgram>> programs_;
    FxFrameContext frameContext_;
};

} // namespace dayo::fx
