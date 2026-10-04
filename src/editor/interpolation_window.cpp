#include "editor/interpolation_window.hpp"
#include "editor/editor_session.hpp"

#include <algorithm>
#include <cmath>

namespace dayo::editor {
namespace {
template <typename Key> CurveEditState readCurve(const Key& key, std::size_t axis) {
    CurveEditState state;
    state.method = key.methods[axis];
    for (std::size_t point = 0; point < 4; ++point) {
        if constexpr (std::tuple_size_v<decltype(key.interpolation)> == 64)
            state.points[point] = key.interpolation[axis + point * 4];
        else
            state.points[point] = key.interpolation[axis * 4 + point];
    }
    return state;
}
template <typename Key> void writeCurve(Key& key, std::size_t axis, const CurveEditState& state) {
    key.methods[axis] = state.method;
    for (std::size_t point = 0; point < 4; ++point) {
        if constexpr (std::tuple_size_v<decltype(key.interpolation)> == 64) {
            key.interpolation[axis + point * 4] = state.points[point];
            // Keep all four VMD byte copies consistent with the canonical block.
            for (std::size_t copy = 1; copy < 4; ++copy)
                key.interpolation[copy * 16 + axis + point * 4] = state.points[point];
        } else
            key.interpolation[axis * 4 + point] = state.points[point];
    }
}
} // namespace
void InterpolationWindow::setCurve(CurveEditState curve) noexcept {
    for (auto& value : curve.points)
        value = std::min<std::uint8_t>(value, 127);
    curve.method = curve.method == 1 ? 1 : 0;
    curve_ = curve;
}
float InterpolationWindow::evaluate(float t) const noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    // Invert cubic X, then evaluate Y; drawing Y(t) directly is not VMD Bezier.
    const auto cubic = [](float u, float a, float b) {
        const auto inverse = 1.0F - u;
        return 3.0F * inverse * inverse * u * a + 3.0F * inverse * u * u * b + u * u * u;
    };
    float low = 0.0F, high = 1.0F;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const float u = (low + high) * 0.5F;
        if (cubic(u, static_cast<float>(curve_.points[0]) / 127.0F, static_cast<float>(curve_.points[2]) / 127.0F) < t)
            low = u;
        else
            high = u;
    }
    return cubic((low + high) * 0.5F, static_cast<float>(curve_.points[1]) / 127.0F,
                 static_cast<float>(curve_.points[3]) / 127.0F);
}
std::optional<CurveEditState> InterpolationWindow::selectedCurve(EditorSession& session, std::size_t axis) const {
    const auto* scene = session.scene();
    const auto* motion = scene ? scene->motion(session.target(), session.global()) : nullptr;
    if (!motion)
        return std::nullopt;
    const auto document = core::toMotionDocument(*motion);
    std::optional<CurveEditState> result;
    for (const auto id : session.selection().ids()) {
        const auto index = session.stableIds().resolve(document, id);
        if (!index)
            continue;
        std::optional<CurveEditState> candidate;
        if (id.track == core::MotionTrack::bone && axis < 4)
            candidate = readCurve(document.bones[*index], axis);
        if (id.track == core::MotionTrack::camera && axis < 6)
            candidate = readCurve(document.cameras[*index], axis);
        if (!candidate)
            continue;
        if (result && *result != *candidate)
            return std::nullopt;
        result = candidate;
    }
    return result;
}
std::size_t InterpolationWindow::commit(EditorSession& session, std::size_t axis, bool allAxes) {
    const auto* scene = session.scene();
    const auto* motion = scene ? scene->motion(session.target(), session.global()) : nullptr;
    if (!motion)
        return 0;
    auto document = core::toMotionDocument(*motion);
    if (document.interpolation != core::InterpolationMode::bezier) {
        const bool catmull = document.interpolation == core::InterpolationMode::catmullRom;
        const auto preserve = [&](auto& keys, std::size_t axes) {
            for (auto& key : keys)
                for (std::size_t channel = 0; channel < axes; ++channel) {
                    CurveEditState state;
                    state.method = catmull ? 1 : 0;
                    writeCurve(key, channel, state);
                }
        };
        preserve(document.bones, 4);
        preserve(document.cameras, 6);
    }
    std::size_t changed = 0;
    for (const auto id : session.selection().ids()) {
        const auto index = session.stableIds().resolve(document, id);
        if (!index)
            continue;
        const auto apply = [&](auto& key, std::size_t axes) {
            if (!allAxes && axis >= axes)
                return;
            for (std::size_t channel = 0; channel < axes; ++channel)
                if (allAxes || channel == axis)
                    writeCurve(key, channel, curve_);
            ++changed;
        };
        if (id.track == core::MotionTrack::bone)
            apply(document.bones[*index], 4);
        if (id.track == core::MotionTrack::camera)
            apply(document.cameras[*index], 6);
    }
    if (changed != 0) {
        document.interpolation = core::InterpolationMode::bezier;
        session.operations().push(ReplaceMotionOperation{session.target(), session.global(),
                                                         core::toVmdMotion(std::move(document), motion->modelName),
                                                         "Edit key interpolation"});
    }
    return changed;
}
void InterpolationWindow::commitMethod(EditorSession& session) {
    if (session.selection().empty() && target_.stableId != 0)
        session.selection().set({target_});
    static_cast<void>(commit(session, axis_));
}
} // namespace dayo::editor
