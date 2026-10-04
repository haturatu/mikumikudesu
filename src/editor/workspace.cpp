#include "editor/workspace.hpp"
#include "editor/model_commands.hpp"
#include "editor/ui_labels.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#if DAYO_HAS_IMGUI
// clang-format off
#include <imgui.h>
#include <ImGuizmo.h>
// clang-format on
#endif
namespace dayo::editor {
void Workspace::drawHistory(EditorSession& session) {
#if DAYO_HAS_IMGUI
    if (ImGui::Begin("Manipulation history")) {
        if (ImGui::Button(uiLabel("Undo")))
            session.history()->undo(*session.scene());
        ImGui::SameLine();
        if (ImGui::Button(uiLabel("Redo")))
            session.history()->redo(*session.scene());
        ImGui::SeparatorText("Applied (newest first)");
        for (const auto& name : session.history()->undoNames())
            ImGui::TextUnformatted(name.c_str());
        ImGui::SeparatorText("Redo");
        for (const auto& name : session.history()->redoNames())
            ImGui::TextDisabled("%s", name.c_str());
    }
    ImGui::End();
#else
    static_cast<void>(session);
#endif
}
bool Workspace::drawModels(EditorSession& session, float frame) {
#if DAYO_HAS_IMGUI
    bool changed = false;
    auto& scene = *session.scene();
    auto* model = scene.selectedModel();
    if (!ImGui::Begin("Models / External parents")) {
        ImGui::End();
        return false;
    }
    if (model) {
        bool visible = model->animationVisible;
        if (ImGui::Checkbox(uiLabel("Animation visible"), &visible)) {
            auto motion = model->motion ? *model->motion : core::VmdMotion{};
            auto document = core::toMotionDocument(motion);
            const auto keyFrame = static_cast<std::uint32_t>(std::max(frame, 0.0F));
            auto found =
                std::ranges::find_if(document.ik, [keyFrame](const auto& key) { return key.frame == keyFrame; });
            if (found == document.ik.end()) {
                core::VmdIkKey key;
                key.frame = keyFrame;
                key.visible = visible;
                document.ik.push_back(key);
            } else
                found->visible = visible;
            core::MotionEditor::normalize(document);
            session.setTarget(model->id, false);
            session.operations().push(ReplaceMotionOperation{
                model->id, false, core::toVmdMotion(std::move(document), motion.modelName), "Register visibility"});
        }
        ImGui::TextDisabled("This creates a visibility key at the current frame. Editor visibility is separate.");
        if (ImGui::Button(uiLabel("Delete selected model")))
            ImGui::OpenPopup("Delete model?");
        if (ImGui::BeginPopupModal("Delete model?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete %s? Undo restores the model.", model->displayName.c_str());
            if (ImGui::Button(uiLabel("Delete"))) {
                session.cancelKeyframeDrag();
                session.history()->execute(scene, std::make_unique<DeleteModelCommand>(scene, model->id));
                model = scene.selectedModel();
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(uiLabel("Cancel")))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (ImGui::BeginTabBar("Model execution order")) {
        const char* stages[]{"Motion", "Deform", "Postprocess", "Raster"};
        for (int stage = 0; stage < 4; ++stage)
            if (ImGui::BeginTabItem(stages[stage])) {
                std::vector<core::ModelId> ids;
                for (const auto& item : scene.models())
                    ids.push_back(item.id);
                const auto rank = [&](core::ModelId id) {
                    const auto& order = scene.model(id)->order;
                    switch (stage) {
                    case 0:
                        return order.motion;
                    case 1:
                        return order.deform;
                    case 2:
                        return order.postprocess;
                    default:
                        return order.raster;
                    }
                };
                std::stable_sort(ids.begin(), ids.end(), [&](auto a, auto b) { return rank(a) < rank(b); });
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    const auto id = ids[index];
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(scene.model(id)->displayName.c_str(), scene.selectedModelId() == id)) {
                        scene.selectModel(id);
                        model = scene.selectedModel();
                    }
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("DAYO_MODEL_ORDER", &id, sizeof(id));
                        ImGui::TextUnformatted(scene.model(id)->displayName.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const auto* payload = ImGui::AcceptDragDropPayload("DAYO_MODEL_ORDER")) {
                            core::ModelId source{};
                            if (payload->DataSize == sizeof(source)) {
                                std::memcpy(&source, payload->Data, sizeof(source));
                                const auto found = std::ranges::find(ids, source);
                                if (found != ids.end() && source != id) {
                                    ids.erase(found);
                                    ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(std::min(index, ids.size())),
                                               source);
                                    session.history()->execute(
                                        scene, std::make_unique<SetModelOrdersCommand>(scene, ids, stage));
                                    changed = true;
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }
    ImGui::SeparatorText("External parents");
    if (model && model->model) {
        const auto bonePicker = [](const char* label, const core::ModelInstance& item, int& index) {
            const auto& bones = item.model->bones;
            if (bones.empty())
                return false;
            index = std::clamp(index, 0, static_cast<int>(bones.size() - 1));
            bool selected = false;
            if (ImGui::BeginCombo(label, bones[static_cast<std::size_t>(index)].name.c_str())) {
                for (std::size_t i = 0; i < bones.size(); ++i)
                    if (ImGui::Selectable((bones[i].name + "##" + std::to_string(i)).c_str(),
                                          index == static_cast<int>(i))) {
                        index = static_cast<int>(i);
                        selected = true;
                    }
                ImGui::EndCombo();
            }
            return selected;
        };
        bonePicker("Child bone", *model, childBone_);
        if (!scene.model(parentModel_))
            parentModel_ = scene.models().empty() ? 0 : scene.models().front().id;
        const auto* parent = scene.model(parentModel_);
        if (ImGui::BeginCombo(uiLabel("Parent model"), parent ? parent->displayName.c_str() : "None")) {
            for (const auto& item : scene.models())
                if (ImGui::Selectable((item.displayName + "##" + std::to_string(item.id)).c_str(),
                                      item.id == parentModel_)) {
                    parentModel_ = item.id;
                    parentBone_ = 0;
                }
            ImGui::EndCombo();
        }
        parent = scene.model(parentModel_);
        if (parent)
            bonePicker("Parent bone", *parent, parentBone_);
        ImGui::BeginDisabled(!parent || model->model->bones.empty() || parent->model->bones.empty());
        if (ImGui::Button(uiLabel("Register external parent")) && parent && !parent->model->bones.empty() &&
            !model->model->bones.empty()) {
            auto links = scene.effectiveExternalParents(frame);
            const auto& child = model->model->bones[static_cast<std::size_t>(childBone_)].name;
            std::erase_if(links,
                          [&](const auto& link) { return link.childModel == model->id && link.childBone == child; });
            links.push_back({parent->id, parent->model->bones[static_cast<std::size_t>(parentBone_)].name, model->id,
                             model->model->bones[static_cast<std::size_t>(childBone_)].name});
            try {
                session.history()->execute(
                    scene, std::make_unique<ExternalParentsCommand>(scene, std::move(links),
                                                                    static_cast<std::uint32_t>(std::max(frame, 0.0F))));
                parentError_.clear();
                changed = true;
            } catch (const std::exception& error) {
                parentError_ = error.what();
            }
        }
        ImGui::EndDisabled();
    }
    if (!parentError_.empty())
        ImGui::TextWrapped("%s", parentError_.c_str());
    if (ImGui::BeginTable("External parent links", 4, ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Child");
        ImGui::TableSetupColumn("Parent model");
        ImGui::TableSetupColumn("Parent bone");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        const auto effectiveLinks = scene.effectiveExternalParents(frame);
        for (std::size_t i = 0; i < effectiveLinks.size(); ++i) {
            const auto& link = effectiveLinks[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(link.childBone.c_str());
            ImGui::TableNextColumn();
            const auto* parent = scene.model(link.parentModel);
            ImGui::TextUnformatted(parent ? parent->displayName.c_str() : "Missing");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(link.parentBone.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(uiLabel("Delete"))) {
                auto links = scene.effectiveExternalParents(frame);
                links.erase(links.begin() + static_cast<std::ptrdiff_t>(i));
                session.history()->execute(
                    scene, std::make_unique<ExternalParentsCommand>(scene, std::move(links),
                                                                    static_cast<std::uint32_t>(std::max(frame, 0.0F))));
                changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
    return changed;
#else
    static_cast<void>(session);
    static_cast<void>(frame);
    return false;
#endif
}
bool Workspace::manipulating() const noexcept {
#if DAYO_HAS_IMGUI
    return ImGuizmo::IsUsing() || boxSelecting_;
#else
    return false;
#endif
}
bool Workspace::drawBonePanel(EditorSession& session, PoseBinding& binding, core::ModelInstance& model, float frame) {
#if DAYO_HAS_IMGUI
    bool changed = false;
    const auto& bones = model.model->bones;
    const auto active = binding.activeBone();
    if (ImGui::BeginCombo(uiLabel("Bone"),
                          active >= 0 ? bones[static_cast<std::size_t>(active)].name.c_str() : "Select bone")) {
        for (std::size_t index = 0; index < bones.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(bones[index].name.c_str(),
                                  std::ranges::find(binding.selectedBones(), static_cast<int>(index)) !=
                                      binding.selectedBones().end()))
                binding.selectBone(static_cast<int>(index), ImGui::GetIO().KeyShift, ImGui::GetIO().KeyCtrl);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::Text("%zu bones selected", binding.selectedBones().size());
    ImGui::Checkbox(uiLabel("World coordinates"), &world_);
    ImGui::SameLine();
    ImGui::Checkbox(uiLabel("Rotation gizmo"), &rotate_);
    auto* edit = binding.activeEdit();
    if (!edit)
        return false;
    const auto& selectedBone = bones[static_cast<std::size_t>(active)];
    if ((selectedBone.flags & (0x0020U | 0x0100U | 0x0200U)) != 0)
        ImGui::TextWrapped("IK / inherited bones: use local numeric editing. The world gizmo is disabled.");
    ImGui::TextDisabled("Numeric fields are VMD local inputs; Local/World controls the gizmo.");
    auto translation = edit->translation;
    if (ImGui::DragFloat3(uiLabel("XYZ position"), translation.data(), 0.01F)) {
        core::Float3 delta{};
        for (std::size_t axis = 0; axis < 3; ++axis)
            delta[axis] = translation[axis] - edit->translation[axis];
        binding.translateSelected(delta, model, false);
        changed = true;
    }
    auto rotation = edit->rotation;
    if (ImGui::DragFloat4(uiLabel("Quaternion"), rotation.data(), 0.005F, -1.0F, 1.0F)) {
        binding.rotateSelected(multiplyRotation(inverseRotation(edit->rotation), rotation), model, false);
        changed = true;
    }
    if (ImGui::Checkbox(uiLabel("Bone physics"), &edit->physics)) {
        binding.setPhysicsSelected(edit->physics);
        changed = true;
    }
    if (ImGui::Button(uiLabel("Register bone keys"))) {
        session.setTarget(model.id, false);
        binding.registerBones(session, static_cast<std::uint32_t>(std::max(frame, 0.0F)));
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(uiLabel("Revert"))) {
        binding.revertSelected();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(uiLabel("Init"))) {
        binding.initializeSelected();
        changed = true;
    }
    ImGui::TextDisabled("Shift: add selection; Ctrl: toggle. Register saves the preview as keys.");
    return changed;
#else
    static_cast<void>(session);
    static_cast<void>(binding);
    static_cast<void>(model);
    static_cast<void>(frame);
    return false;
#endif
}
void Workspace::drawInterpolation(EditorSession& session, InterpolationWindow& window) {
#if DAYO_HAS_IMGUI
    if (!ImGui::Begin("Interpolation")) {
        ImGui::End();
        return;
    }
    const bool camera = std::ranges::any_of(session.selection().ids(),
                                            [](const auto id) { return id.track == core::MotionTrack::camera; });
    curveAxis_ = std::clamp(curveAxis_, 0, camera ? 5 : 3);
    ImGui::Combo(uiLabel("Axis"), &curveAxis_, camera ? "X\0Y\0Z\0Rotation\0Distance\0FoV\0" : "X\0Y\0Z\0Rotation\0");
    const auto current = window.selectedCurve(session, static_cast<std::size_t>(curveAxis_));
    const auto revision = session.scene()->motionRevision();
    if (curveSelection_ != session.selection().ids() || curveRevision_ != revision || loadedAxis_ != curveAxis_) {
        curveSelection_ = session.selection().ids();
        curveRevision_ = revision;
        loadedAxis_ = curveAxis_;
        window.setCurve(current.value_or(CurveEditState{}));
        curveDirty_ = false;
    }
    if (!current)
        ImGui::TextDisabled("Mixed values, or no editable bone/camera keys selected.");
    auto state = window.curve();
    int method = state.method;
    if (ImGui::Combo(uiLabel("Method"), &method, "VMD Bezier\0Catmull-Rom\0")) {
        state.method = static_cast<std::uint8_t>(method);
        curveDirty_ = true;
    }
    int points[4]{state.points[0], state.points[1], state.points[2], state.points[3]};
    ImGui::BeginDisabled(method != 0);
    if (ImGui::SliderInt4(uiLabel("x1 y1 x2 y2"), points, 0, 127)) {
        for (std::size_t i = 0; i < 4; ++i)
            state.points[i] = static_cast<std::uint8_t>(points[i]);
        curveDirty_ = true;
    }
    ImGui::EndDisabled();
    window.setCurve(state);
    if (method == 0) {
        const auto origin = ImGui::GetCursorScreenPos();
        const ImVec2 size{160, 160};
        ImGui::Dummy(size);
        auto* list = ImGui::GetWindowDrawList();
        list->AddRect(origin, {origin.x + size.x, origin.y + size.y}, ImGui::GetColorU32(ImGuiCol_Border));
        ImVec2 previous{origin.x, origin.y + size.y};
        for (int i = 1; i <= 64; ++i) {
            const float t = static_cast<float>(i) / 64.0F;
            const ImVec2 point{origin.x + t * size.x, origin.y + (1.0F - window.evaluate(t)) * size.y};
            list->AddLine(previous, point, ImGui::GetColorU32(ImGuiCol_CheckMark), 2);
            previous = point;
        }
    } else
        ImGui::TextWrapped("Catmull-Rom uses neighboring keys; Bezier control points are retained.");
    ImGui::Checkbox(uiLabel("All axes"), &allAxes_);
    if (ImGui::Button(uiLabel("Copy curve")))
        curveClipboard_ = window.curve();
    ImGui::SameLine();
    ImGui::BeginDisabled(!curveClipboard_);
    if (ImGui::Button(uiLabel("Paste curve"))) {
        window.setCurve(*curveClipboard_);
        curveDirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(uiLabel("Init curve"))) {
        window.setCurve({});
        curveDirty_ = true;
    }
    ImGui::BeginDisabled(session.selection().empty());
    if (ImGui::Button(uiLabel("Register interpolation"))) {
        static_cast<void>(window.commit(session, static_cast<std::size_t>(curveAxis_), allAxes_));
        curveDirty_ = false;
    }
    ImGui::EndDisabled();
    if (curveDirty_)
        ImGui::TextDisabled("Unregistered curve changes");
    ImGui::End();
#else
    static_cast<void>(session);
    static_cast<void>(window);
#endif
}
bool Workspace::drawViewport(PoseBinding& binding, const core::ModelInstance& model,
                             const graphics::SceneCameraMatrices& camera, ScreenRect viewport, bool hovered,
                             bool rigidBodies) {
#if DAYO_HAS_IMGUI
    const auto* poses = binding.poses(model.id);
    if (!poses || !model.model)
        return false;
    struct Hit {
        int bone;
        ScreenPoint screen;
    };
    std::vector<Hit> hits;
    auto* list = ImGui::GetWindowDrawList();
    for (std::size_t i = 0; i < poses->size(); ++i) {
        auto position = (*poses)[i].worldPosition;
        for (std::size_t axis = 0; axis < 3; ++axis)
            position[axis] = (position[axis] - model.normalization.center[axis]) * model.normalization.scale;
        const auto screen = projectToViewport(position, camera.viewProjection, viewport);
        if (!screen.visible)
            continue;
        hits.push_back({static_cast<int>(i), screen});
        const bool selected =
            std::ranges::find(binding.selectedBones(), static_cast<int>(i)) != binding.selectedBones().end();
        list->AddCircleFilled({screen.x, screen.y}, selected ? 5.0F : 3.0F,
                              ImGui::GetColorU32(selected ? ImGuiCol_PlotHistogram : ImGuiCol_CheckMark));
    }
    if (rigidBodies) {
        const auto normalize = [&](core::Float3 point) {
            for (std::size_t axis = 0; axis < 3; ++axis)
                point[axis] = (point[axis] - model.normalization.center[axis]) * model.normalization.scale;
            return point;
        };
        for (std::size_t index = 0; index < model.model->rigidBodies.size(); ++index) {
            const auto& body = model.model->rigidBodies[index];
            core::PhysicsTransform pose;
            pose.position = body.position;
            const auto quaternion = [](std::size_t axis, float angle) {
                core::Float4 result{0, 0, 0, std::cos(angle * 0.5F)};
                result[axis] = std::sin(angle * 0.5F);
                return result;
            };
            pose.rotation =
                multiplyRotation(quaternion(2, body.rotation[2]),
                                 multiplyRotation(quaternion(1, body.rotation[1]), quaternion(0, body.rotation[0])));
            if (model.physics && model.physics->available() && index < model.physics->bodyCount())
                pose = model.physics->bodyTransform(index);
            const auto line = [&](core::Float3 a, core::Float3 b) {
                a = rotatePoint(pose.rotation, a);
                b = rotatePoint(pose.rotation, b);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    a[axis] += pose.position[axis];
                    b[axis] += pose.position[axis];
                }
                const auto first = projectToViewport(normalize(a), camera.viewProjection, viewport),
                           last = projectToViewport(normalize(b), camera.viewProjection, viewport);
                if (first.visible && last.visible)
                    list->AddLine({first.x, first.y}, {last.x, last.y}, ImGui::GetColorU32(ImGuiCol_PlotLines));
            };
            if (body.shape == 1) {
                for (int mask = 0; mask < 8; ++mask)
                    for (int axis = 0; axis < 3; ++axis)
                        if ((mask & (1 << axis)) == 0) {
                            core::Float3 a{}, b{};
                            for (std::size_t i = 0; i < 3; ++i)
                                a[i] = ((mask & (1 << i)) ? 1.0F : -1.0F) * body.size[i];
                            b = a;
                            b[static_cast<std::size_t>(axis)] = -b[static_cast<std::size_t>(axis)];
                            line(a, b);
                        }
            } else {
                const float radius = body.size[0], halfHeight = body.shape == 2 ? body.size[1] * 0.5F : 0;
                for (int plane = 0; plane < 3; ++plane)
                    for (int segment = 0; segment < 32; ++segment)
                        for (int cap = 0; cap < (body.shape == 2 ? 2 : 1); ++cap) {
                            const float first = static_cast<float>(segment) * 6.2831853F / 32,
                                        last = static_cast<float>(segment + 1) * 6.2831853F / 32;
                            core::Float3 a{}, b{};
                            const auto axis = static_cast<std::size_t>(plane), next = (axis + 1) % 3;
                            a[axis] = std::cos(first) * radius;
                            a[next] = std::sin(first) * radius;
                            b[axis] = std::cos(last) * radius;
                            b[next] = std::sin(last) * radius;
                            a[1] += cap == 0 ? -halfHeight : halfHeight;
                            b[1] += cap == 0 ? -halfHeight : halfHeight;
                            line(a, b);
                        }
                if (body.shape == 2)
                    for (int side = 0; side < 4; ++side) {
                        const float angle = static_cast<float>(side) * 1.5707963F;
                        line({std::cos(angle) * radius, -halfHeight, std::sin(angle) * radius},
                             {std::cos(angle) * radius, halfHeight, std::sin(angle) * radius});
                    }
            }
        }
    }
    bool changed = false;
    const auto active = binding.activeBone();
    auto* edit = binding.activeEdit();
    bool safe = edit && active >= 0;
    for (const auto index : binding.selectedBones()) {
        const auto& bone = model.model->bones[static_cast<std::size_t>(index)];
        if ((bone.flags & (rotate_ ? 0x0002U : 0x0004U)) == 0)
            safe = false;
        if ((bone.flags & (0x0020U | 0x0100U | 0x0200U)) != 0)
            safe = false;
        for (const auto& source : model.model->bones)
            if (std::ranges::any_of(source.ikLinks, [&](const auto& link) { return link.bone == index; }))
                safe = false;
        for (const auto& body : model.model->rigidBodies)
            if (body.bone == index && body.mode != 0 && binding.edit(index) && binding.edit(index)->physics)
                safe = false;
    }
    if (safe && static_cast<std::size_t>(active) < poses->size()) {
        const auto& pose = (*poses)[static_cast<std::size_t>(active)];
        auto position = pose.worldPosition;
        for (std::size_t axis = 0; axis < 3; ++axis)
            position[axis] = (position[axis] - model.normalization.center[axis]) * model.normalization.scale;
        auto matrix = poseMatrix(position, pose.rotation);
        const auto original = matrix;
        ImGuizmo::SetDrawlist(list);
        ImGuizmo::SetRect(viewport.x, viewport.y, viewport.width, viewport.height);
        ImGuizmo::SetOrthographic(camera.projection[15] != 0.0F);
        if (ImGuizmo::Manipulate(camera.view.data(), camera.projection.data(),
                                 rotate_ ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE,
                                 world_ ? ImGuizmo::WORLD : ImGuizmo::LOCAL, matrix.data())) {
            if (rotate_)
                binding.rotateSelected(
                    multiplyRotation(matrixRotation(matrix), inverseRotation(matrixRotation(original))), model, true);
            else {
                core::Float3 delta{};
                for (std::size_t axis = 0; axis < 3; ++axis)
                    delta[axis] = (matrix[12 + axis] - original[12 + axis]) / model.normalization.scale;
                binding.translateSelected(delta, model, true);
            }
            changed = true;
        }
    }
    const auto& io = ImGui::GetIO();
    if (hovered && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Hit* closest = nullptr;
        float distance = 64.0F;
        for (const auto& hit : hits) {
            const float dx = hit.screen.x - io.MousePos.x, dy = hit.screen.y - io.MousePos.y;
            const float squared = dx * dx + dy * dy;
            if (squared < distance) {
                distance = squared;
                closest = &hit;
            }
        }
        if (closest)
            binding.selectBone(closest->bone, io.KeyShift, io.KeyCtrl);
        else {
            boxSelecting_ = true;
            boxBegin_ = {io.MousePos.x, io.MousePos.y, 0, true};
            boxOriginal_ = binding.selectedBones();
        }
    }
    if (boxSelecting_) {
        const ImVec2 minimum{std::min(boxBegin_.x, io.MousePos.x), std::min(boxBegin_.y, io.MousePos.y)};
        const ImVec2 maximum{std::max(boxBegin_.x, io.MousePos.x), std::max(boxBegin_.y, io.MousePos.y)};
        list->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_CheckMark));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            boxSelecting_ = false;
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            boxSelecting_ = false;
            auto selected = io.KeyCtrl || io.KeyShift ? boxOriginal_ : std::vector<int>{};
            for (const auto& hit : hits)
                if (hit.screen.x >= minimum.x && hit.screen.x <= maximum.x && hit.screen.y >= minimum.y &&
                    hit.screen.y <= maximum.y) {
                    const auto found = std::ranges::find(selected, hit.bone);
                    if (io.KeyCtrl && found != selected.end())
                        selected.erase(found);
                    else if (found == selected.end())
                        selected.push_back(hit.bone);
                }
            binding.setBones(std::move(selected));
        }
    }
    return changed;
#else
    static_cast<void>(binding);
    static_cast<void>(model);
    static_cast<void>(camera);
    static_cast<void>(viewport);
    static_cast<void>(hovered);
    static_cast<void>(rigidBodies);
    return false;
#endif
}
} // namespace dayo::editor
