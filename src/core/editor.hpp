#pragma once

#include "core/scene.hpp"

#include <memory>
#include <cstdint>
#include <string_view>
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

enum class MotionTrack { bone, morph, camera, light, shadow, ik };

struct MotionKeyRef {
    MotionTrack track {};
    std::size_t index {};
    friend bool operator==(const MotionKeyRef&, const MotionKeyRef&) = default;
};

struct MotionClipboard {
    MotionDocument keys;
    std::uint32_t originFrame {};
    [[nodiscard]] bool empty() const noexcept;
};

class MotionEditor {
public:
    static void normalize(MotionDocument& document);
    static void erase(MotionDocument& document, std::vector<MotionKeyRef> keys);
    static void move(MotionDocument& document, const std::vector<MotionKeyRef>& keys, std::int64_t frameDelta);
    [[nodiscard]] static MotionClipboard copy(const MotionDocument& document,
                                              const std::vector<MotionKeyRef>& keys);
    [[nodiscard]] static std::vector<MotionKeyRef> paste(MotionDocument& document,
                                                         const MotionClipboard& clipboard,
                                                         std::uint32_t destinationFrame);
};

// One snapshot command covers key insertion/deletion/move, pose, morph,
// camera, light, shadow, IK, interpolation and VPD application atomically.
class EditMotionCommand final : public EditCommand {
public:
    EditMotionCommand(ModelId target, bool global, VmdMotion before, VmdMotion after,
                      std::string label = "Edit motion")
        : target_(target), global_(global), before_(std::move(before)), after_(std::move(after)),
          label_(std::move(label)) {}
    void apply(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] const char* name() const noexcept override { return label_.c_str(); }

private:
    ModelId target_ {};
    bool global_ {};
    VmdMotion before_;
    VmdMotion after_;
    std::string label_;
};

} // namespace dayo::core
