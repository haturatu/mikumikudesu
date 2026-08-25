#pragma once

#include "core/scene.hpp"

#include <memory>
#include <string>
#include <vector>

namespace dayo::core {

class EditCommand {
public:
    virtual ~EditCommand() = default;
    virtual void apply(Scene& scene) = 0;
    virtual void undo(Scene& scene) = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

class CommandHistory {
public:
    void execute(Scene& scene, std::unique_ptr<EditCommand> command);
    bool undo(Scene& scene);
    bool redo(Scene& scene);
    void clear() noexcept;
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undoCount() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redoCount() const noexcept { return redo_.size(); }

private:
    std::vector<std::unique_ptr<EditCommand>> undo_;
    std::vector<std::unique_ptr<EditCommand>> redo_;
};

class SetFrameCommand final : public EditCommand {
public:
    SetFrameCommand(float before, float after) : before_(before), after_(after) {}
    void apply(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] const char* name() const noexcept override { return "Set frame"; }

private:
    static void set(Scene& scene, float frame);
    float before_ {};
    float after_ {};
};

class SetRuntimeModeCommand final : public EditCommand {
public:
    SetRuntimeModeCommand(RuntimeMode before, RuntimeMode after) : before_(before), after_(after) {}
    void apply(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] const char* name() const noexcept override { return "Set runtime mode"; }

private:
    RuntimeMode before_;
    RuntimeMode after_;
};

} // namespace dayo::core
