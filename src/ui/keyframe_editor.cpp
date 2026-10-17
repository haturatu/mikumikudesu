#include "ui/keyframe_editor.hpp"

#include "ui/keyframe_edit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_map>

namespace dayo::ui {
namespace {

const char* trackName(core::MotionTrack track) {
    switch (track) {
    case core::MotionTrack::bone:
        return "Bone";
    case core::MotionTrack::morph:
        return "Morph";
    case core::MotionTrack::camera:
        return "Camera";
    case core::MotionTrack::light:
        return "Light";
    case core::MotionTrack::shadow:
        return "Self shadow";
    case core::MotionTrack::ik:
        return "IK / visibility";
    }
    return "Key";
}

bool contains(std::string text, std::string query) {
    const auto lower = [](unsigned char value) { return static_cast<char>(std::tolower(value)); };
    std::transform(text.begin(), text.end(), text.begin(), lower);
    std::transform(query.begin(), query.end(), query.begin(), lower);
    return text.find(query) != std::string::npos;
}

std::uint32_t frameNumber(float frame) {
    return static_cast<std::uint32_t>(
        std::clamp(static_cast<double>(frame), 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

core::Float3 rotationDegrees(const core::Float4& q) {
    constexpr float degrees = 180.0F / std::numbers::pi_v<float>;
    return {degrees * std::atan2(2.0F * (q[3] * q[0] + q[1] * q[2]), 1.0F - 2.0F * (q[0] * q[0] + q[1] * q[1])),
            degrees * std::asin(std::clamp(2.0F * (q[3] * q[1] - q[2] * q[0]), -1.0F, 1.0F)),
            degrees * std::atan2(2.0F * (q[3] * q[2] + q[0] * q[1]), 1.0F - 2.0F * (q[1] * q[1] + q[2] * q[2]))};
}

core::Float4 rotationQuaternion(const core::Float3& angles) {
    constexpr float halfRadians = std::numbers::pi_v<float> / 360.0F;
    const float cx = std::cos(angles[0] * halfRadians), sx = std::sin(angles[0] * halfRadians);
    const float cy = std::cos(angles[1] * halfRadians), sy = std::sin(angles[1] * halfRadians);
    const float cz = std::cos(angles[2] * halfRadians), sz = std::sin(angles[2] * halfRadians);
    return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz};
}

void hint(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

} // namespace

void KeyframeEditor::clearSelection() {
    selection_.clear();
    selectedSet_.clear();
    anchor_.reset();
    draft_.reset();
    draftKey_.reset();
    groupOffset_ = 0;
    keyGesture_ = boxGesture_ = false;
    dragBefore_.reset();
    dragDelta_ = 0;
}

void KeyframeEditor::synchronize(core::Scene& scene, core::ModelId target, bool global) {
    const auto* current = scene.motion(target, global);
    const bool targetChanged = target_ != target || global_ != global;
    if (targetChanged ||
        ((revision_ != scene.motionRevision() || current != motion_) && ownRevision_ != scene.motionRevision()))
        clearSelection();
    if (targetChanged) {
        startFrame_ = 0;
        zoom_ = 1;
    }
    target_ = target;
    global_ = global;
    if (targetChanged || revision_ != scene.motionRevision() || motion_ != current) {
        motion_ = current;
        revision_ = scene.motionRevision();
        rebuild(scene);
    }
}

void KeyframeEditor::rebuild(const core::Scene& scene) {
    rows_.clear();
    if (motion_ == nullptr)
        return;
    const auto group = [&](const auto& keys, core::MotionTrack track) {
        std::unordered_map<std::string, std::size_t> lookup;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            std::string name;
            if constexpr (requires { keys[i].name; })
                name = keys[i].name;
            else
                name = trackName(track);
            const auto [it, inserted] = lookup.emplace(name, rows_.size());
            if (inserted)
                rows_.push_back({track, name, {}, {}});
            rows_[it->second].keys.push_back({{track, i}, keys[i].frame});
        }
    };
    group(motion_->bones, core::MotionTrack::bone);
    group(motion_->morphs, core::MotionTrack::morph);
    group(motion_->cameras, core::MotionTrack::camera);
    group(motion_->lights, core::MotionTrack::light);
    group(motion_->shadows, core::MotionTrack::shadow);
    group(motion_->ik, core::MotionTrack::ik);
    std::unordered_map<std::string, std::size_t> order;
    if (const auto* model = scene.model(target_); model != nullptr && model->model != nullptr && !global_) {
        const auto metadata = [&](const auto& values, core::MotionTrack track) {
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto& value = values[i];
                order.emplace(std::to_string(static_cast<int>(track)) + value.name, i);
                for (auto& row : rows_)
                    if (row.track == track && row.name == value.name)
                        row.alias = value.englishName;
            }
        };
        metadata(model->model->bones, core::MotionTrack::bone);
        metadata(model->model->morphs, core::MotionTrack::morph);
    }
    const auto rank = [&](const Row& row) {
        const auto it = order.find(std::to_string(static_cast<int>(row.track)) + row.name);
        return it == order.end() ? std::numeric_limits<std::size_t>::max() : it->second;
    };
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        return a.track == b.track ? rank(a) < rank(b) : a.track < b.track;
    });
    for (auto& row : rows_)
        std::stable_sort(row.keys.begin(), row.keys.end(), [](Entry a, Entry b) { return a.frame < b.frame; });
}

bool KeyframeEditor::matches(const Row& row) const {
    return (trackFilter_ == 0 || static_cast<int>(row.track) + 1 == trackFilter_) &&
           (filter_[0] == '\0' || contains(row.name, filter_.data()) || contains(row.alias, filter_.data()));
}

void KeyframeEditor::select(Entry key, const Row& row, bool toggle, bool range) {
    draft_.reset();
    if (range && anchor_ && anchor_->ref.track == key.ref.track) {
        if (!toggle)
            selection_.clear();
        const auto first = std::min(anchor_->frame, key.frame), last = std::max(anchor_->frame, key.frame);
        for (const auto entry : row.keys)
            if (entry.frame >= first && entry.frame <= last &&
                std::find(selection_.begin(), selection_.end(), entry.ref) == selection_.end())
                selection_.push_back(entry.ref);
    } else {
        const auto found = std::find(selection_.begin(), selection_.end(), key.ref);
        if (toggle && found != selection_.end())
            selection_.erase(found);
        else if (found == selection_.end()) {
            if (!toggle)
                selection_.clear();
            selection_.push_back(key.ref);
        }
        anchor_ = key;
    }
    selectedSet_.clear();
    selectedSet_.insert(selection_.begin(), selection_.end());
}

void KeyframeEditor::execute(core::Scene& scene, core::CommandHistory& history, core::VmdMotion after,
                             const char* label) {
    const auto* before = scene.motion(target_, global_);
    if (before == nullptr)
        return;
    updateMotionDuration(after);
    history.execute(scene,
                    std::make_unique<core::EditMotionCommand>(target_, global_, *before, std::move(after), label));
    ownRevision_ = scene.motionRevision();
    draft_.reset();
    synchronize(scene, target_, global_);
    if (anchor_)
        if (const auto frame = keyFrame(*motion_, anchor_->ref))
            anchor_->frame = *frame;
}

bool KeyframeEditor::moveSelection(core::Scene& scene, core::CommandHistory& history, std::int64_t delta) {
    if (motion_ == nullptr || selection_.empty() || delta == 0)
        return false;
    auto after = *motion_;
    if (moveKeyframes(after, selection_, delta) == 0)
        return false;
    execute(scene, history, std::move(after), "Move selected keys");
    return true;
}

KeyframeEditor::Result KeyframeEditor::draw(core::Scene& scene, core::CommandHistory& history, float frame) {
    synchronize(scene, target_, global_);
    Result result;
    ImGui::SetNextItemWidth(145.0F);
    ImGui::InputTextWithHint("##track-search", "Find track / English name", filter_.data(), filter_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115.0F);
    ImGui::Combo("##track-type", &trackFilter_, "All tracks\0Bones\0Morphs\0Camera\0Light\0Shadow\0IK\0");
    ImGui::SameLine();
    ImGui::Checkbox("List", &listVisible_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) {
        zoom_ = 1;
        startFrame_ = 0;
    }
    hint("Fit the full motion. Ctrl+wheel zooms around the pointer; middle-drag pans.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(65.0F);
    if (ImGui::DragFloat("Zoom", &zoom_, 0.1F, 1.0F, 500.0F, "%.1fx", ImGuiSliderFlags_AlwaysClamp))
        startFrame_ = std::max(0.0F, frame - scene.timeline().duration / (2.0F * zoom_));
    if (motion_ == nullptr) {
        ImGui::TextWrapped("Load a model and a VMD, or choose Camera / light to edit global keys.");
        return result;
    }
    const bool shortcuts = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                           !ImGui::GetIO().WantTextInput && !keyGesture_ && !boxGesture_;
    const auto shortcut = [&](ImGuiKey key) { return shortcuts && ImGui::IsKeyPressed(key, false); };
    if (ImGui::GetIO().KeyCtrl && shortcut(ImGuiKey_A)) {
        clearSelection();
        for (const auto& row : rows_)
            if (matches(row))
                for (const auto key : row.keys)
                    selection_.push_back(key.ref);
        selectedSet_.insert(selection_.begin(), selection_.end());
    }
    if (shortcut(ImGuiKey_Escape))
        clearSelection();
    ImGui::BeginDisabled(selection_.empty());
    if (ImGui::SmallButton("Copy") || (ImGui::GetIO().KeyCtrl && shortcut(ImGuiKey_C)))
        clipboard_ = core::MotionEditor::copy(core::toMotionDocument(*motion_), selection_);
    hint("Copy selected keys (Ctrl+C)");
    ImGui::SameLine();
    const bool cut = ImGui::SmallButton("Cut") || (ImGui::GetIO().KeyCtrl && shortcut(ImGuiKey_X));
    ImGui::SameLine();
    const bool remove = ImGui::SmallButton("Delete") || shortcut(ImGuiKey_Delete);
    ImGui::SameLine();
    if (ImGui::SmallButton("-1f") || (!ImGui::GetIO().KeyCtrl && shortcut(ImGuiKey_LeftArrow)))
        result.changed |= moveSelection(scene, history, ImGui::GetIO().KeyShift ? -10 : -1);
    hint("Move selection left (Left arrow; Shift = 10 frames)");
    ImGui::SameLine();
    if (ImGui::SmallButton("+1f") || (!ImGui::GetIO().KeyCtrl && shortcut(ImGuiKey_RightArrow)))
        result.changed |= moveSelection(scene, history, ImGui::GetIO().KeyShift ? 10 : 1);
    hint("Move selection right (Right arrow; Shift = 10 frames)");
    ImGui::EndDisabled();
    if ((cut || remove) && !selection_.empty()) {
        auto document = core::toMotionDocument(*motion_);
        if (cut)
            clipboard_ = core::MotionEditor::copy(document, selection_);
        core::MotionEditor::erase(document, selection_);
        auto after = core::toVmdMotion(std::move(document), motion_->modelName);
        clearSelection();
        execute(scene, history, std::move(after), cut ? "Cut keys" : "Delete keys");
        result.changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(clipboard_.empty());
    const bool paste = ImGui::SmallButton("Paste") || (ImGui::GetIO().KeyCtrl && shortcut(ImGuiKey_V));
    ImGui::EndDisabled();
    if (paste && !clipboard_.empty()) {
        auto document = core::toMotionDocument(*motion_);
        static_cast<void>(core::MotionEditor::paste(document, clipboard_, frameNumber(frame)));
        auto after = core::toVmdMotion(std::move(document), motion_->modelName);
        clearSelection();
        execute(scene, history, std::move(after), "Paste keys");
        result.changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Prev key")) {
        float previous = 0;
        for (const auto& row : rows_)
            if (matches(row))
                for (const auto entry : row.keys)
                    if (static_cast<float>(entry.frame) < frame)
                        previous = std::max(previous, static_cast<float>(entry.frame));
        result.seek = previous;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Next key")) {
        float next = scene.timeline().duration;
        for (const auto& row : rows_)
            if (matches(row))
                for (const auto entry : row.keys)
                    if (static_cast<float>(entry.frame) > frame)
                        next = std::min(next, static_cast<float>(entry.frame));
        result.seek = next;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu selected", selection_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("More..."))
        ImGui::OpenPopup("motion-options");
    if (ImGui::BeginPopup("motion-options")) {
        int interpolation = static_cast<int>(motion_->interpolation);
        if (ImGui::Combo("Motion interpolation", &interpolation, "Linear\0VMD Bezier\0Catmull-Rom\0")) {
            auto after = *motion_;
            after.interpolation = static_cast<core::InterpolationMode>(interpolation);
            execute(scene, history, std::move(after), "Set motion interpolation");
            result.changed = true;
        }
        ImGui::InputTextWithHint("##vmd-export-path", "Destination .vmd path", exportPath_.data(), exportPath_.size());
        ImGui::BeginDisabled(exportPath_[0] == '\0');
        if (ImGui::Button("Export VMD")) {
            try {
                core::saveVmd(exportPath_.data(), *motion_);
                status_ = "VMD exported";
            } catch (const std::exception& error) {
                status_ = error.what();
            }
        }
        ImGui::EndDisabled();
        if (!status_.empty())
            ImGui::TextWrapped("%s", status_.c_str());
        ImGui::EndPopup();
    }
    const float footerHeight = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##keyframe-area", {0, std::max(60.0F, ImGui::GetContentRegionAvail().y - footerHeight)},
                          ImGuiChildFlags_Borders)) {
        if (listVisible_)
            drawList(frame, result);
        else
            drawCanvas(scene, history, frame, result);
    }
    ImGui::EndChild();
    finishGesture(scene, history, result);
    if (result.changed && selection_.size() == 1)
        if (const auto selectedFrame = keyFrame(*motion_, selection_.front()))
            result.seek = static_cast<float>(*selectedFrame);
    ImGui::TextDisabled("Click: select  |  Ctrl: add  |  Shift: range  |  Drag: move / box  |  Esc: cancel");
    result.pause |= result.changed || result.seek.has_value();
    return result;
}

void KeyframeEditor::drawCanvas(core::Scene& scene, core::CommandHistory& history, float frame, Result& result) {
    static_cast<void>(history);
    visibleKeys_.clear();
    std::vector<std::size_t> filtered;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (matches(rows_[i]))
            filtered.push_back(i);
    if (filtered.empty()) {
        ImGui::TextDisabled("No matching tracks. Clear the search or change the track type.");
        return;
    }
    const auto flags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##keyframe-tracks", 2, flags))
        return;
    ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 170.0F);
    ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupScrollFreeze(1, 1);
    rowHeight_ = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0F + 4.0F;
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, rowHeight_);
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%zu tracks", filtered.size());
    ImGui::TableSetColumnIndex(1);
    const auto ruler = ImGui::GetCursorScreenPos();
    const float width = std::max(20.0F, ImGui::GetContentRegionAvail().x);
    const float duration = std::max(scene.timeline().duration, 1.0F);
    pixelsPerFrame_ = width / duration * zoom_;
    startFrame_ = std::clamp(startFrame_, 0.0F, std::max(0.0F, duration - width / pixelsPerFrame_));
    const auto frameX = [&](float value) { return ruler.x + (value - startFrame_) * pixelsPerFrame_; };
    const auto underPointer = [&] { return startFrame_ + (ImGui::GetIO().MousePos.x - ruler.x) / pixelsPerFrame_; };
    ImGui::InvisibleButton("##frame-ruler", {width, ImGui::GetTextLineHeight() + 4.0F});
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        result.seek = std::round(std::clamp(underPointer(), 0.0F, duration));
    auto* draw = ImGui::GetWindowDrawList();
    float step = 1;
    while (step * pixelsPerFrame_ < 65.0F)
        step *= 10.0F;
    if (step * pixelsPerFrame_ > 325.0F)
        step /= 5.0F;
    else if (step * pixelsPerFrame_ > 130.0F)
        step /= 2.0F;
    const auto tickStep = static_cast<std::uint64_t>(std::max(step, 1.0F));
    const auto firstTick =
        static_cast<std::uint64_t>(std::ceil(static_cast<double>(startFrame_) / static_cast<double>(tickStep))) *
        tickStep;
    const auto lastTick = static_cast<std::uint64_t>(startFrame_ + width / pixelsPerFrame_);
    for (auto tick = firstTick; tick <= lastTick; tick += tickStep) {
        const std::string label = std::to_string(tick);
        draw->AddText({frameX(static_cast<float>(tick)) + 2, ruler.y}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      label.c_str());
    }
    canvasOrigin_ = {ruler.x, ruler.y + rowHeight_};
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(filtered.size()), rowHeight_);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto& row = rows_[filtered[static_cast<std::size_t>(index)]];
            ImGui::PushID(index);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight_);
            ImGui::TableSetColumnIndex(0);
            const bool rowSelected =
                !selection_.empty() && std::any_of(row.keys.begin(), row.keys.end(),
                                                   [&](Entry key) { return selectedSet_.contains(key.ref); });
            if (ImGui::Selectable(row.name.c_str(), rowSelected, ImGuiSelectableFlags_None,
                                  {0, ImGui::GetTextLineHeight() + 4.0F})) {
                const auto nearest = std::min_element(row.keys.begin(), row.keys.end(), [&](Entry a, Entry b) {
                    return std::abs(static_cast<float>(a.frame) - frame) <
                           std::abs(static_cast<float>(b.frame) - frame);
                });
                select(*nearest, row, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
                result.seek = static_cast<float>(nearest->frame);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s  %s\n%zu keys · %s", row.name.c_str(), row.alias.c_str(), row.keys.size(),
                                  trackName(row.track));
            ImGui::TableSetColumnIndex(1);
            const auto cell = ImGui::GetCursorScreenPos();
            if (index == 0)
                canvasOrigin_ = cell;
            const float height = ImGui::GetTextLineHeight() + 4.0F;
            ImGui::InvisibleButton("##keys", {width, height},
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
            const bool hovered = ImGui::IsItemHovered();
            const float centerY = cell.y + height * 0.5F;
            const float visibleFirst = startFrame_ - 8.0F / pixelsPerFrame_;
            const float visibleLast = startFrame_ + (width + 8.0F) / pixelsPerFrame_;
            const auto first =
                std::lower_bound(row.keys.begin(), row.keys.end(), visibleFirst,
                                 [](Entry key, float value) { return static_cast<float>(key.frame) < value; });
            float lastPixel = -1e10F;
            for (auto it = first; it != row.keys.end() && static_cast<float>(it->frame) <= visibleLast; ++it) {
                const bool selected = selectedSet_.contains(it->ref);
                const float ghost = selected && keyGesture_ ? static_cast<float>(dragDelta_) : 0;
                const float px = frameX(static_cast<float>(it->frame) + ghost);
                visibleKeys_.push_back({*it, {frameX(static_cast<float>(it->frame)), centerY}});
                if (!selected && px - lastPixel < 3.0F)
                    continue;
                lastPixel = px;
                const ImU32 color =
                    selected ? IM_COL32(255, 205, 95, 255)
                             : ImGui::GetColorU32(row.track == core::MotionTrack::morph ? ImGuiCol_PlotLines
                                                                                        : ImGuiCol_SliderGrab);
                draw->AddQuadFilled({px, centerY - 5}, {px + 5, centerY}, {px, centerY + 5}, {px - 5, centerY}, color);
                if (selected)
                    draw->AddQuad({px, centerY - 6}, {px + 6, centerY}, {px, centerY + 6}, {px - 6, centerY},
                                  IM_COL32(255, 240, 200, 255));
            }
            if (frameX(frame) >= ruler.x && frameX(frame) <= ruler.x + width)
                draw->AddLine({frameX(frame), cell.y}, {frameX(frame), cell.y + height}, IM_COL32(255, 205, 95, 210),
                              2);
            if (hovered) {
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                const auto& io = ImGui::GetIO();
                if (io.MouseWheel != 0 && io.KeyCtrl) {
                    const float fixedFrame = underPointer();
                    zoom_ = std::clamp(zoom_ * std::pow(1.3F, io.MouseWheel), 1.0F, 500.0F);
                    startFrame_ = std::max(0.0F, fixedFrame - (io.MousePos.x - ruler.x) / (width / duration * zoom_));
                } else if (io.MouseWheel != 0 && io.KeyShift) {
                    startFrame_ = std::clamp(startFrame_ - io.MouseWheel * 60.0F / pixelsPerFrame_, 0.0F, duration);
                } else if (io.MouseWheel != 0) {
                    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * rowHeight_ * 3);
                }
                const auto nearest =
                    std::lower_bound(row.keys.begin(), row.keys.end(), underPointer(),
                                     [](Entry key, float value) { return static_cast<float>(key.frame) < value; });
                std::optional<Entry> hit;
                float distance = 8;
                const auto consider = [&](auto it) {
                    const float delta = std::abs(frameX(static_cast<float>(it->frame)) - io.MousePos.x);
                    if (delta <= distance) {
                        hit = *it;
                        distance = delta;
                    }
                };
                if (nearest != row.keys.end())
                    consider(nearest);
                if (nearest != row.keys.begin())
                    consider(std::prev(nearest));
                if (hit)
                    ImGui::SetTooltip("%s · frame %u\nDrag to move; edit values in Inspector", row.name.c_str(),
                                      hit->frame);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    gestureStart_ = io.MousePos;
                    if (hit) {
                        select(*hit, row, io.KeyCtrl, io.KeyShift);
                        result.seek = static_cast<float>(hit->frame);
                        if (selectedSet_.contains(hit->ref)) {
                            keyGesture_ = true;
                            dragBefore_ = *motion_;
                            dragDelta_ = 0;
                        }
                    } else {
                        if (!io.KeyCtrl)
                            clearSelection();
                        boxGesture_ = true;
                        gestureStart_ = io.MousePos;
                        result.seek = std::round(std::clamp(underPointer(), 0.0F, duration));
                    }
                }
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                startFrame_ = std::clamp(startFrame_ - ImGui::GetIO().MouseDelta.x / pixelsPerFrame_, 0.0F, duration);
            ImGui::PopID();
        }
    }
    if (boxGesture_) {
        const auto mouse = ImGui::GetIO().MousePos;
        draw->AddRectFilled({std::min(mouse.x, gestureStart_.x), std::min(mouse.y, gestureStart_.y)},
                            {std::max(mouse.x, gestureStart_.x), std::max(mouse.y, gestureStart_.y)},
                            IM_COL32(85, 160, 240, 40));
        draw->AddRect({std::min(mouse.x, gestureStart_.x), std::min(mouse.y, gestureStart_.y)},
                      {std::max(mouse.x, gestureStart_.x), std::max(mouse.y, gestureStart_.y)},
                      IM_COL32(110, 185, 255, 255));
    }
    ImGui::EndTable();
}

void KeyframeEditor::drawList(float frame, Result& result) {
    static_cast<void>(frame);
    for (const auto& row : rows_) {
        if (!matches(row))
            continue;
        ImGui::PushID(static_cast<int>(row.track));
        ImGui::PushID(row.name.c_str());
        ImGui::SeparatorText(row.name.c_str());
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(row.keys.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto entry = row.keys[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                const std::string label = std::string(trackName(row.track)) + " · frame " + std::to_string(entry.frame);
                if (ImGui::Selectable(label.c_str(), selectedSet_.contains(entry.ref))) {
                    select(entry, row, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
                    result.seek = static_cast<float>(entry.frame);
                }
                ImGui::PopID();
            }
        ImGui::PopID();
        ImGui::PopID();
    }
}

void KeyframeEditor::finishGesture(core::Scene& scene, core::CommandHistory& history, Result& result) {
    if (!keyGesture_ && !boxGesture_)
        return;
    const auto& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        keyGesture_ = boxGesture_ = false;
        dragBefore_.reset();
        dragDelta_ = 0;
        return;
    }
    if (keyGesture_ && dragBefore_) {
        auto delta = static_cast<std::int64_t>(std::llround((io.MousePos.x - gestureStart_.x) / pixelsPerFrame_));
        std::uint32_t first = std::numeric_limits<std::uint32_t>::max(), last = 0;
        for (const auto ref : selection_)
            if (const auto value = keyFrame(*dragBefore_, ref)) {
                first = std::min(first, *value);
                last = std::max(last, *value);
            }
        delta = std::clamp(delta, -static_cast<std::int64_t>(first),
                           static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) - last);
        if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            delta = 0;
        dragDelta_ = delta;
        if (delta != 0)
            ImGui::SetTooltip("Move %+.0f frames · release to apply · Esc to cancel", static_cast<double>(delta));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto after = std::move(*dragBefore_);
            dragBefore_.reset();
            keyGesture_ = false;
            dragDelta_ = 0;
            if (moveKeyframes(after, selection_, delta) != 0) {
                execute(scene, history, std::move(after), "Drag keys");
                result.changed = result.pause = true;
            }
        }
    }
    if (boxGesture_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float left = std::min(gestureStart_.x, io.MousePos.x), right = std::max(gestureStart_.x, io.MousePos.x);
        const float top = std::min(gestureStart_.y, io.MousePos.y), bottom = std::max(gestureStart_.y, io.MousePos.y);
        for (const auto& key : visibleKeys_)
            if (key.position.x >= left && key.position.x <= right && key.position.y >= top &&
                key.position.y <= bottom && selectedSet_.insert(key.key.ref).second)
                selection_.push_back(key.key.ref);
        boxGesture_ = false;
        draft_.reset();
    }
}

bool KeyframeEditor::drawInspector(core::Scene& scene, core::CommandHistory& history) {
    synchronize(scene, target_, global_);
    if (selection_.empty() || motion_ == nullptr)
        return false;
    ImGui::SeparatorText("Selected keyframe");
    if (selection_.size() != 1) {
        ImGui::Text("%zu keys selected", selection_.size());
        ImGui::TextWrapped("Drag to move together, use -1f / +1f for precision, or Shift+arrow for 10 frames.");
        ImGui::SetNextItemWidth(-1);
        ImGui::TextUnformatted("Offset (frames)");
        ImGui::InputInt("##group-offset", &groupOffset_);
        ImGui::BeginDisabled(groupOffset_ == 0);
        const bool apply = ImGui::Button("Move selected keys");
        ImGui::EndDisabled();
        if (apply) {
            const bool changed = moveSelection(scene, history, groupOffset_);
            groupOffset_ = 0;
            return changed;
        }
        return false;
    }
    const auto ref = selection_.front();
    if (!keyFrame(*motion_, ref)) {
        clearSelection();
        return false;
    }
    if (!draft_ || draftKey_ != ref) {
        draft_ = *motion_;
        draftKey_ = ref;
    }
    ImGui::TextDisabled("%s · %s", global_ ? "Camera / light" : "Model motion", trackName(ref.track));
    std::string name;
    visitKey(*draft_, ref, [&](const auto& key) {
        if constexpr (requires { key.name; })
            name = key.name;
    });
    if (!name.empty())
        ImGui::TextWrapped("%s", name.c_str());
    ImGui::PushItemWidth(-1);
    visitKey(*draft_, ref, [&](auto& key) {
        ImGui::TextUnformatted("Frame");
        ImGui::InputScalar("##key-frame", ImGuiDataType_U32, &key.frame);
    });
    const auto vector3 = [](const char* label, core::Float3& value) {
        ImGui::TextUnformatted(label);
        ImGui::DragFloat3((std::string("##key-") + label).c_str(), value.data(), 0.01F, 0, 0, "%.3f");
    };
    switch (ref.track) {
    case core::MotionTrack::bone: {
        auto& key = draft_->bones[ref.index];
        vector3("Position", key.translation);
        auto degrees = rotationDegrees(key.rotation);
        ImGui::TextUnformatted("Rotation (degrees)");
        if (ImGui::DragFloat3("##key-rotation", degrees.data(), 0.1F, 0, 0, "%.2f"))
            key.rotation = rotationQuaternion(degrees);
        ImGui::Checkbox("Physics enabled", &key.physics);
        if (ImGui::CollapsingHeader("Key interpolation (advanced)")) {
            constexpr std::array<const char*, 4> axes{"X", "Y", "Z", "Rotation"};
            for (std::size_t axis = 0; axis < axes.size(); ++axis) {
                std::array<int, 4> points{};
                for (std::size_t point = 0; point < 4; ++point)
                    points[point] = key.interpolation[axis + point * 4];
                if (ImGui::DragInt4(axes[axis], points.data(), 1, 0, 127, "%d", ImGuiSliderFlags_AlwaysClamp))
                    for (std::size_t point = 0; point < 4; ++point)
                        key.interpolation[axis + point * 4] = static_cast<std::uint8_t>(points[point]);
            }
        }
        break;
    }
    case core::MotionTrack::morph:
        ImGui::TextUnformatted("Weight");
        ImGui::SliderFloat("##key-weight", &draft_->morphs[ref.index].weight, 0, 1, "%.3f");
        break;
    case core::MotionTrack::camera: {
        auto& key = draft_->cameras[ref.index];
        vector3("Target", key.position);
        auto rotation = key.rotation;
        for (auto& axis : rotation)
            axis *= 180.0F / std::numbers::pi_v<float>;
        ImGui::TextUnformatted("Rotation (degrees)");
        if (ImGui::DragFloat3("##key-camera-rotation", rotation.data(), 0.1F)) {
            for (auto& axis : rotation)
                axis *= std::numbers::pi_v<float> / 180.0F;
            key.rotation = rotation;
        }
        ImGui::TextUnformatted("Distance");
        ImGui::DragFloat("##key-distance", &key.distance, .1F);
        ImGui::TextUnformatted("Field of view");
        ImGui::SliderFloat("##key-fov", &key.viewAngle, 1, 179);
        ImGui::Checkbox("Perspective", &key.perspective);
        break;
    }
    case core::MotionTrack::light:
        ImGui::TextUnformatted("Color");
        ImGui::ColorEdit3("##key-color", draft_->lights[ref.index].color.data());
        vector3("Direction", draft_->lights[ref.index].position);
        break;
    case core::MotionTrack::shadow: {
        auto& key = draft_->shadows[ref.index];
        int mode = key.mode;
        ImGui::Combo("Mode##key-shadow", &mode, "None\0Mode 1\0Mode 2\0");
        key.mode = static_cast<std::uint8_t>(mode);
        ImGui::TextUnformatted("Distance");
        ImGui::DragFloat("##key-shadow-distance", &key.distance, .1F, 0, 10000);
        break;
    }
    case core::MotionTrack::ik: {
        auto& key = draft_->ik[ref.index];
        ImGui::Checkbox("Model visible", &key.visible);
        for (std::size_t i = 0; i < key.states.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox(key.states[i].name.c_str(), &key.states[i].enabled);
            ImGui::PopID();
        }
        break;
    }
    }
    ImGui::PopItemWidth();
    bool valid = true;
    visitKey(*draft_, ref, [&](const auto& key) {
        const auto finite = [](const auto& values) {
            return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
        };
        if constexpr (requires { key.translation; })
            valid &= finite(key.translation);
        if constexpr (requires { key.rotation; })
            valid &= finite(key.rotation);
        if constexpr (requires { key.position; })
            valid &= finite(key.position);
        if constexpr (requires { key.color; })
            valid &= finite(key.color);
        if constexpr (requires { key.weight; })
            valid &= std::isfinite(key.weight);
        if constexpr (requires { key.distance; })
            valid &= std::isfinite(key.distance);
        if constexpr (requires { key.viewAngle; })
            valid &= std::isfinite(key.viewAngle);
    });
    if (!valid)
        ImGui::TextWrapped("Enter finite numeric values before applying this key.");
    ImGui::TextWrapped("Apply saves this key as one undo step. Ctrl+Z restores the previous values.");
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Apply key")) {
        auto after = std::move(*draft_);
        execute(scene, history, std::move(after), "Edit selected key");
        ImGui::EndDisabled();
        return true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        draft_.reset();
    ImGui::Separator();
    return false;
}

} // namespace dayo::ui
