#include "core/cache_key.hpp"
#include "core/editor.hpp"
#include "core/fx_debug.hpp"
#include "core/image_hdr.hpp"
#include "core/physics_profile.hpp"
#include "core/sequence_path.hpp"
#include "core/softbody_native.hpp"
#include "core/solver.hpp"
#include "editor/editor_operation.hpp"
#include "editor/editor_session.hpp"
#include "editor/motion_key_id.hpp"
#include "editor/selection.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

} // namespace

int main() {
    bool ok = true;

    // Stable ID: sorting by frame must not invalidate (track, stableId).
    {
        dayo::core::MotionDocument document;
        document.bones.push_back({.name = "arm", .frame = 20});
        document.bones.push_back({.name = "leg", .frame = 5});
        dayo::editor::StableIdTable table;
        table.rebuild(document);
        const auto armId = table.keyId(dayo::core::MotionTrack::bone, 0);
        const auto legId = table.keyId(dayo::core::MotionTrack::bone, 1);
        ok &= check(armId.stableId != 0 && legId.stableId != 0 && armId.stableId != legId.stableId,
                    "stable ids are unique per key");
        dayo::core::MotionEditor::normalize(document);
        table.rebuild(document);
        const auto armIndex = table.resolve(document, armId);
        const auto legIndex = table.resolve(document, legId);
        ok &= check(armIndex.has_value() && legIndex.has_value() && document.bones[*armIndex].name == "arm" &&
                        document.bones[*legIndex].name == "leg",
                    "stable ids survive frame-order sort");
        // Transient index use only: selection resolves per frame.
        dayo::editor::Selection selection;
        selection.set({armId});
        const auto refs = selection.resolveTransient(document, table);
        ok &= check(refs.size() == 1 && document.bones[refs.front().index].name == "arm",
                    "selection resolves stable id transiently");

        dayo::core::MotionDocument duplicates;
        duplicates.bones.push_back({.name = "dup", .frame = 3});
        duplicates.bones.push_back({.name = "dup", .frame = 3});
        dayo::editor::StableIdTable duplicateTable;
        duplicateTable.rebuild(duplicates);
        const auto firstDuplicate = duplicateTable.keyId(dayo::core::MotionTrack::bone, 0);
        const auto secondDuplicate = duplicateTable.keyId(dayo::core::MotionTrack::bone, 1);
        dayo::core::MotionEditor::normalize(duplicates);
        duplicateTable.rebuild(duplicates);
        ok &= check(duplicateTable.resolve(duplicates, firstDuplicate).has_value() &&
                        duplicateTable.resolve(duplicates, secondDuplicate).has_value() &&
                        firstDuplicate != secondDuplicate,
                    "stable ids retain duplicate key ordinals");
    }

    // Undo transaction: drag coalesces into a single history entry.
    {
        dayo::core::Scene scene;
        dayo::core::CommandHistory history;
        dayo::core::VmdMotion initial;
        initial.bones.push_back({.name = "arm", .frame = 0});
        scene.replaceMotion(initial, 0, true);
        {
            dayo::editor::UndoTransaction transaction(scene, history, 0, true, "Drag keys");
            dayo::core::VmdMotion dragged = initial;
            dragged.bones.push_back({.name = "arm", .frame = 10});
            transaction.dragTo(dragged);
            dragged.bones.push_back({.name = "arm", .frame = 20});
            transaction.dragTo(dragged);
            transaction.commit();
        }
        const auto* current = scene.motion(0, true);
        ok &= check(current != nullptr && current->bones.size() == 3 && history.undoCount() == 1,
                    "drag transaction coalesces into one undo entry");
        ok &= check(history.undo(scene) && scene.motion(0, true)->bones.size() == 1,
                    "coalesced drag undoes to pre-drag state");
    }

    // EditorOperation queue: UI path mutates only via history.
    {
        dayo::core::Scene scene;
        dayo::core::CommandHistory history;
        dayo::editor::EditorOperationQueue queue;
        queue.push(dayo::editor::SetFrameOperation{12.0F});
        ok &= check(queue.flush(scene, history) == 1 && scene.timeline().frame == 12.0F && history.undoCount() == 1,
                    "operation queue applies SetFrame through history");
    }

    // Sequence path spec: parse + format round trip.
    {
        const auto parsed = dayo::core::parseSequencePath("frame_000003.ppm");
        ok &= check(parsed.has_value() && parsed->prefix == "frame_" && parsed->digits == 6 && parsed->start == 3 &&
                        parsed->extension == ".ppm",
                    "sequence path parses prefix/digits/extension");
        ok &= check(!dayo::core::parseSequencePath("frame.ppm").has_value(), "sequence path rejects digit-less names");
        if (parsed.has_value()) {
            const auto formatted = dayo::core::formatSequencePath("out", *parsed, 18);
            ok &= check(formatted == std::filesystem::path("out/frame_000018.ppm"), "sequence path formats frames");
        }
    }

    // MoveKeysOperation resolves stable ids and records a real history command.
    {
        dayo::core::Scene scene;
        dayo::core::CommandHistory history;
        dayo::core::VmdMotion initial;
        initial.bones.push_back({.name = "arm", .frame = 5});
        scene.replaceMotion(initial, 0, true);
        dayo::editor::StableIdTable table;
        table.rebuild(dayo::core::toMotionDocument(initial));
        const auto id = table.keyId(dayo::core::MotionTrack::bone, 0);
        dayo::editor::EditorOperationQueue queue;
        queue.setStableIdTable(table);
        dayo::editor::MoveKeysOperation move;
        move.target = 0;
        move.global = true;
        move.keys = {id};
        move.frameDelta = 7;
        queue.push(std::move(move));
        ok &= check(queue.flush(scene, history) == 1 && history.undoCount() == 1 &&
                        scene.motion(0, true)->bones.front().frame == 12,
                    "MoveKeysOperation changes motion through history");
        ok &= check(history.undo(scene) && scene.motion(0, true)->bones.front().frame == 5,
                    "MoveKeysOperation undo restores motion");

        const dayo::editor::StableIdTable copiedTable = table;
        dayo::editor::EditorOperationQueue copiedQueue;
        copiedQueue.setStableIdTable(copiedTable);
        dayo::editor::MoveKeysOperation copiedMove;
        copiedMove.target = 0;
        copiedMove.global = true;
        copiedMove.keys = {id};
        copiedMove.frameDelta = 1;
        copiedQueue.push(copiedMove);
        ok &= check(copiedQueue.flush(scene, history) == 1 && scene.motion(0, true)->bones.front().frame == 6,
                    "const stable id table supports move operations through its internal copy");
    }

    // HDR: half round trip + RGBA16F skeleton.
    {
        dayo::core::ImageRgba8 ldr{2, 1, {255, 0, 0, 255, 0, 255, 0, 255}};
        const auto half = dayo::core::rgba8ToHalf(ldr);
        ok &= check(half.type == dayo::core::PixelType::half16 && half.byteSize() == 2U * 4U * sizeof(std::uint16_t),
                    "RGBA16F half image layout");
        const auto roundTrip = dayo::core::halfToRgba8(half);
        ok &= check(roundTrip.pixels == ldr.pixels, "half round trip preserves LDR bytes");
        const float one = dayo::core::halfToFloat(dayo::core::floatToHalf(1.0F));
        ok &= check(std::abs(one - 1.0F) < 0.001F, "half conversion near 1.0");
        ok &= check(dayo::core::halfToFloat(0x0001U) > 0.0F, "half conversion preserves subnormal values");
        ok &=
            check(std::isinf(dayo::core::halfToFloat(dayo::core::floatToHalf(std::numeric_limits<float>::infinity()))),
                  "half conversion preserves infinity");

        dayo::core::ImageData unorm{
            1, 1, 4, dayo::core::PixelType::unorm8, dayo::core::ColorSpace::srgb, {128, 64, 32, 255}};
        const auto linearHalf =
            dayo::core::convertImage(unorm, dayo::core::PixelType::half16, dayo::core::ColorSpace::linear);
        ok &= check(linearHalf.bytes.size() == 4U * sizeof(std::uint16_t) &&
                        linearHalf.space == dayo::core::ColorSpace::linear,
                    "HDR conversion allocates target half storage");
        std::uint16_t linearRedBits{};
        std::memcpy(&linearRedBits, linearHalf.bytes.data(), sizeof(linearRedBits));
        ok &= check(dayo::core::halfToFloat(linearRedBits) < 0.22F, "HDR conversion applies sRGB to linear transfer");
        const auto floatImage =
            dayo::core::convertImage(linearHalf, dayo::core::PixelType::float32, dayo::core::ColorSpace::linear);
        float convertedRed{};
        std::memcpy(&convertedRed, floatImage.bytes.data(), sizeof(convertedRed));
        ok &= check(convertedRed > 0.0F && floatImage.bytes.size() == 4U * sizeof(float),
                    "HDR conversion writes real float32 samples");
        dayo::core::ImageData spoofed = unorm;
        spoofed.type = dayo::core::PixelType::float32;
        spoofed.bytes.resize(4);
        ok &= check(
            [&] {
                try {
                    static_cast<void>(dayo::core::convertImage(spoofed, dayo::core::PixelType::half16,
                                                               dayo::core::ColorSpace::linear));
                } catch (const std::invalid_argument&) {
                    return true;
                }
                return false;
            }(),
            "HDR conversion rejects metadata-only type spoofing");
    }

    // Solver smoke: documented pipeline runs + camera chain separated.
    {
        dayo::core::VmdMotion motion;
        motion.bones.push_back({.name = "arm", .frame = 0});
        motion.bones.push_back({.name = "arm", .frame = 10});
        dayo::core::VmdCameraKey camera;
        camera.frame = 0;
        camera.distance = -5.0F;
        motion.cameras.push_back(camera);
        dayo::core::MotionSolver solver;
        const auto result = solver.solveModel(motion, 5.0F);
        ok &= check(result.lastStage == dayo::core::SolverStage::final && !result.bones.empty() &&
                        result.camera.distance == -5.0F,
                    "solver smoke runs model chain plus separate camera chain");
        ok &= check(solver.compareBones(result.bones, result.bones, nullptr), "solver tolerance self-compare");
        dayo::core::VmdCameraState shifted = result.camera;
        shifted.distance += 1.0F;
        ok &= check(!solver.compareCamera(result.camera, shifted, nullptr), "solver camera tolerance rejects drift");
    }

    // Physics profile: fixed-step accumulator + prewarm/seek determinism.
    {
        dayo::core::PhysicsStepper stepper;
        stepper.seekTo(0.0F);
        const auto steps = stepper.stepFixed(1.0F / 30.0F);
        ok &= check(steps == 4 && stepper.accumulatedSteps() == 4, "physics fixed-step accumulates 120Hz substeps");
        dayo::core::PhysicsStepper capped({}, 1.0e-6F);
        const auto cappedSteps = capped.stepFixed(0.25F);
        ok &= check(cappedSteps > 0 && cappedSteps <= 4096, "physics fixed-step caps hostile substep counts");
        stepper.prewarm(8);
        ok &= check(stepper.accumulatedSteps() == 12, "physics prewarm advances without moving frame clock");
        dayo::core::PhysicsCompatibilityProfile compat;
        compat.useFrameOffset = true;
        compat.stableJoints = true;
        ok &= check(compat.solvePolicy == dayo::core::PhysicsSolvePolicy::sequentialImpulse,
                    "physics compatibility defaults pin sequential impulse");
        dayo::core::PmxModel emptyModel;
        dayo::core::BulletSoftBodyNative native(emptyModel);
        ok &= check(native.usingFallback(), "native softbody path keeps fallback by default");
    }

    // Cache keys: determinism across repeated digests.
    {
        dayo::core::ShaderCacheKey shader;
        shader.source = "preview.hlsl";
        shader.entry = "VS";
        shader.profile = "vs_6_6";
        ok &= check(shader.digest() == shader.digest() && !shader.digest().empty(), "shader cache key deterministic");
        dayo::core::PipelineCacheKey pipeline;
        pipeline.vertex = shader;
        pipeline.fragment = shader;
        pipeline.width = 1920;
        pipeline.height = 1080;
        ok &= check(pipeline.digest() == pipeline.digest(), "pipeline cache key deterministic");
        dayo::core::TextureCacheKey texture{"model/texture.png", 512, 512, "RGBA8"};
        ok &= check(texture.digest() == texture.digest(), "texture cache key deterministic");
        dayo::core::TextureCacheKey other{"model/other.png", 512, 512, "RGBA8"};
        ok &= check(texture.digest() != other.digest(), "texture cache key distinguishes paths");
    }

    // FX debug snapshot: inspector never touches handles.
    {
        dayo::core::EffectGraph graph;
        dayo::core::EffectPass pass;
        pass.name = "opaque";
        graph.passes.push_back(pass);
        const auto snapshot = dayo::core::FxRuntimeInspector::snapshot(graph, 7);
        ok &= check(snapshot.passCount == 1 && snapshot.passNames.front() == "opaque" &&
                        !dayo::core::FxRuntimeInspector::format(snapshot).empty(),
                    "fx debug snapshot inspects graph summary");
    }

    if (ok)
        std::cout << "editor_physics_io: all checks passed\n";
    return ok ? 0 : 1;
}
