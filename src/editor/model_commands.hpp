#pragma once
#include "core/editor.hpp"
namespace dayo::editor {
class DeleteModelCommand final : public core::EditCommand {
  public:
    DeleteModelCommand(const core::Scene& scene, core::ModelId id);
    void apply(core::Scene& scene) override;
    void undo(core::Scene& scene) override;
    const char* name() const noexcept override {
        return "Delete model";
    }

  private:
    core::ModelId id_{};
    core::ModelId selection_{};
    std::size_t index_{};
    std::optional<core::ModelInstance> removed_;
    std::vector<core::ExternalParentLink> links_;
};
class SetModelOrdersCommand final : public core::EditCommand {
  public:
    SetModelOrdersCommand(const core::Scene& scene, const std::vector<core::ModelId>& ids, int stage);
    void apply(core::Scene& scene) override;
    void undo(core::Scene& scene) override;
    const char* name() const noexcept override {
        return "Reorder models";
    }

  private:
    void set(core::Scene& scene, bool after);
    std::vector<core::ModelId> ids_;
    std::vector<core::ModelExecutionOrder> before_, after_;
};
class ExternalParentsCommand final : public core::EditCommand {
  public:
    ExternalParentsCommand(const core::Scene& scene, std::vector<core::ExternalParentLink> links,
                           std::uint32_t frame = 0);
    void apply(core::Scene& scene) override;
    void undo(core::Scene& scene) override;
    const char* name() const noexcept override {
        return "Edit external parents";
    }

  private:
    std::vector<core::ExternalParentLink> before_, after_;
    struct Motion {
        core::ModelId target{};
        core::VmdMotion before, after;
    };
    std::vector<Motion> motions_;
};
} // namespace dayo::editor
