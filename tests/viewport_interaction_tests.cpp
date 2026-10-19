#include "ui/viewport_interaction.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Harness {
    dayo::ui::ViewportInteraction interaction;
    ImVec2 imagePosition{};
    ImVec2 cameraButtonPosition{};
    ImVec2 windowPosition{};
    bool cameraClicked{};

    dayo::ui::ViewportInput frame(ImVec2 mouse, int button = -1, bool navigationEnabled = true, float wheel = 0.0F,
                                  bool ctrl = false, bool shift = false) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, button == ImGuiMouseButton_Left);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, button == ImGuiMouseButton_Right);
        if (wheel != 0.0F)
            io.AddMouseWheelEvent(0.0F, wheel);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20.0F, 20.0F}, ImGuiCond_Once);
        ImGui::SetNextWindowSize({340.0F, 300.0F}, ImGuiCond_Once);
        ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        windowPosition = ImGui::GetWindowPos();
        const auto cameraPosition = ImGui::GetCursorScreenPos();
        cameraButtonPosition = {cameraPosition.x + 10.0F, cameraPosition.y + 10.0F};
        cameraClicked = ImGui::Button("Camera");
        ImGui::SameLine();
        ImGui::Button("Light");
        imagePosition = ImGui::GetCursorScreenPos();
        const auto input = interaction.image(ImTextureRef{ImTextureID{1}}, {300.0F, 200.0F}, navigationEnabled);
        ImGui::End();
        ImGui::Render();
        return input;
    }
};

void checkGestures() {
    Harness harness;
    harness.frame({0.0F, 0.0F});
    const ImVec2 inside{harness.imagePosition.x + 50.0F, harness.imagePosition.y + 50.0F};
    const ImVec2 moved{inside.x + 30.0F, inside.y + 20.0F};
    const ImVec2 outside{500.0F, 500.0F};
    harness.frame(inside);
    expect(harness.frame(inside, ImGuiMouseButton_Left).hovered, "Image must accept the initial left click");
    auto input = harness.frame(moved, ImGuiMouseButton_Left);
    expect(input.orbit && !input.pan, "Left drag must orbit while its own ImGui item is active");
    input = harness.frame(outside, ImGuiMouseButton_Left);
    expect(input.orbit && !input.hovered, "A captured left drag must continue outside the image");
    expect(harness.windowPosition.x == 20.0F && harness.windowPosition.y == 20.0F,
           "Dragging the image must not move the ImGui window");
    expect(!harness.frame(outside).orbit, "Releasing outside must end the captured drag");

    harness.frame(inside);
    harness.frame(inside, ImGuiMouseButton_Right);
    input = harness.frame(moved, ImGuiMouseButton_Right);
    expect(input.pan && !input.orbit, "Right drag must pan");
    expect(harness.frame(outside, ImGuiMouseButton_Right).pan, "A captured right drag must continue outside");
    expect(!harness.frame(inside).contextMenu, "Right drag must not open a context menu on release");

    harness.frame(inside, ImGuiMouseButton_Right);
    harness.frame(moved, ImGuiMouseButton_Right);
    harness.frame(inside, ImGuiMouseButton_Right);
    expect(!harness.frame(inside).contextMenu, "Dragging back to the starting point must not become a click");
    harness.frame(inside, ImGuiMouseButton_Right);
    expect(harness.frame(inside).contextMenu, "A short right click must open the context menu");
    expect(!harness.frame(inside).contextMenu, "A right click must open the context menu only once");

    harness.frame(outside, ImGuiMouseButton_Left);
    expect(!harness.frame(inside, ImGuiMouseButton_Left).orbit, "A drag begun outside must not orbit on entry");
    harness.frame(inside);
    harness.frame(outside, ImGuiMouseButton_Right);
    expect(!harness.frame(inside).contextMenu, "A right click begun outside must not open the menu on entry");

    harness.frame(inside, ImGuiMouseButton_Left, false);
    input = harness.frame(moved, ImGuiMouseButton_Left, false, 1.0F);
    expect(!input.orbit && !input.pan && input.wheel == 0.0F, "VMD follow must lock all camera gestures");
    harness.frame(inside);
    expect(harness.frame(inside, -1, true, 1.0F).wheel == 1.0F, "Wheel must zoom over the image");
    expect(harness.frame(outside, -1, true, 1.0F).wheel == 0.0F, "Wheel outside the image must not zoom");

    harness.frame(inside, -1, false, 0.0F, true);
    harness.frame(inside, ImGuiMouseButton_Left, false, 0.0F, true);
    input = harness.frame(inside, -1, false);
    expect(input.focusPick && !input.orbit && !input.pan, "Ctrl-click focuses even while VMD locks navigation");
    expect(std::abs(input.focusUv.x - 50.0F / 300.0F) < 0.001F && std::abs(input.focusUv.y - 50.0F / 200.0F) < 0.001F,
           "Focus UV uses image coordinates independently of framebuffer DPI");
    expect(!harness.frame(inside).focusPick, "A focus click is emitted only once");
    harness.frame(inside, ImGuiMouseButton_Left, true, 0.0F, true);
    input = harness.frame(moved, ImGuiMouseButton_Left);
    expect(!input.orbit, "Releasing Ctrl mid-gesture must not turn focus into camera orbit");
    expect(!harness.frame(moved).focusPick, "A Ctrl-drag must not focus on release");

    harness.frame(inside, -1, false, 0.0F, true, true);
    harness.frame(inside, ImGuiMouseButton_Left, false, 0.0F, true, true);
    input = harness.frame(inside);
    expect(input.focusPick && input.focusTracking && !input.orbit,
           "Ctrl-Shift-click latches tracking intent even if modifiers are released before the mouse");
    harness.frame(inside);

    const auto cameraButton = harness.cameraButtonPosition;
    harness.frame(cameraButton);
    harness.frame(cameraButton, ImGuiMouseButton_Left);
    input = harness.frame(inside, ImGuiMouseButton_Left);
    expect(!input.orbit && !input.pan, "A toolbar click must not start viewport navigation");
    harness.frame(inside);
    harness.frame(cameraButton);
    harness.frame(cameraButton, ImGuiMouseButton_Left);
    harness.frame(cameraButton);
    expect(harness.cameraClicked, "The Camera toolbar button must remain clickable");
}

} // namespace

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {800.0F, 600.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels{};
    int width{};
    int height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int result = 0;
    try {
        checkGestures();
        std::cout << "Viewport ImGui drag capture, toolbar, context menu and VMD lock passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
