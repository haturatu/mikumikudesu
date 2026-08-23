#include "core/animation.hpp"
#include "core/physics.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <string_view>
#include <unordered_map>

namespace dayo::core {
namespace {

using Quat = Float4;

Float3 add(const Float3& a, const Float3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
Float3 sub(const Float3& a, const Float3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
Float3 mul(const Float3& a, float b) { return { a[0] * b, a[1] * b, a[2] * b }; }
float dot(const Float3& a, const Float3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Float3 cross(const Float3& a, const Float3& b) {
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
float length(const Float3& value) { return std::sqrt(dot(value, value)); }
Float3 normalized(const Float3& value) { const float l = length(value); return l > 1e-8F ? mul(value, 1.0F / l) : Float3 {}; }

Quat normalize(Quat value) {
    const float l = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2] + value[3] * value[3]);
    if (l <= 1e-8F) return { 0.0F, 0.0F, 0.0F, 1.0F };
    for (auto& component : value) component /= l;
    return value;
}
Quat conjugate(const Quat& q) { return { -q[0], -q[1], -q[2], q[3] }; }
Quat multiplyRaw(const Quat& a, const Quat& b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}
Quat multiply(const Quat& a, const Quat& b) {
    return normalize(multiplyRaw(a, b));
}
Float3 rotate(const Quat& q, const Float3& value) {
    const Float3 u { q[0], q[1], q[2] };
    return add(add(mul(u, 2.0F * dot(u, value)), mul(value, q[3] * q[3] - dot(u, u))),
               mul(cross(u, value), 2.0F * q[3]));
}
Quat slerp(Quat a, Quat b, float t) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0F) { for (auto& value : b) value = -value; d = -d; }
    if (d > 0.9995F) {
        for (std::size_t i = 0; i < 4; ++i) a[i] += (b[i] - a[i]) * t;
        return normalize(a);
    }
    const float angle = std::acos(std::clamp(d, -1.0F, 1.0F));
    const float denominator = std::sin(angle);
    const float aWeight = std::sin((1.0F - t) * angle) / denominator;
    const float bWeight = std::sin(t * angle) / denominator;
    for (std::size_t i = 0; i < 4; ++i) a[i] = a[i] * aWeight + b[i] * bWeight;
    return normalize(a);
}
Quat axisAngle(const Float3& axis, float angle) {
    const float half = angle * 0.5F;
    const float sine = std::sin(half);
    return normalize({ axis[0] * sine, axis[1] * sine, axis[2] * sine, std::cos(half) });
}

Quat eulerRotation(const Float3& euler) {
    const auto x = axisAngle({ 1.0F, 0.0F, 0.0F }, euler[0]);
    const auto y = axisAngle({ 0.0F, 1.0F, 0.0F }, euler[1]);
    const auto z = axisAngle({ 0.0F, 0.0F, 1.0F }, euler[2]);
    return multiply(z, multiply(y, x));
}

float bezier(float x, std::uint8_t x1, std::uint8_t y1, std::uint8_t x2, std::uint8_t y2) {
    const float ax = static_cast<float>(x1) / 127.0F;
    const float ay = static_cast<float>(y1) / 127.0F;
    const float bx = static_cast<float>(x2) / 127.0F;
    const float by = static_cast<float>(y2) / 127.0F;
    float low = 0.0F, high = 1.0F;
    for (int i = 0; i < 16; ++i) {
        const float t = (low + high) * 0.5F;
        const float inv = 1.0F - t;
        const float curveX = 3.0F * inv * inv * t * ax + 3.0F * inv * t * t * bx + t * t * t;
        if (curveX < x) low = t; else high = t;
    }
    const float t = (low + high) * 0.5F;
    const float inv = 1.0F - t;
    return 3.0F * inv * inv * t * ay + 3.0F * inv * t * t * by + t * t * t;
}

struct LocalPose { Float3 translation {}; Quat rotation { 0.0F, 0.0F, 0.0F, 1.0F }; };
struct GlobalPose { Float3 position {}; Quat rotation { 0.0F, 0.0F, 0.0F, 1.0F }; };

LocalPose sampleBone(const std::vector<const VmdBoneKey*>& keys, float frame) {
    if (keys.empty()) return {};
    const auto next = std::lower_bound(keys.begin(), keys.end(), frame,
        [](const VmdBoneKey* key, float value) { return static_cast<float>(key->frame) < value; });
    if (next == keys.begin()) return { (*next)->translation, normalize((*next)->rotation) };
    if (next == keys.end()) return { keys.back()->translation, normalize(keys.back()->rotation) };
    const auto* before = *(next - 1);
    const auto* after = *next;
    const float span = static_cast<float>(after->frame - before->frame);
    const float linear = span > 0.0F ? (frame - static_cast<float>(before->frame)) / span : 0.0F;
    LocalPose result;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto offset = axis;
        const float t = bezier(linear, after->interpolation[offset], after->interpolation[offset + 4],
                               after->interpolation[offset + 8], after->interpolation[offset + 12]);
        result.translation[axis] = before->translation[axis] + (after->translation[axis] - before->translation[axis]) * t;
    }
    const float rt = bezier(linear, after->interpolation[3], after->interpolation[7],
                            after->interpolation[11], after->interpolation[15]);
    result.rotation = slerp(before->rotation, after->rotation, rt);
    return result;
}

float sampleMorph(const std::vector<const VmdMorphKey*>& keys, float frame) {
    if (keys.empty()) return 0.0F;
    const auto next = std::lower_bound(keys.begin(), keys.end(), frame,
        [](const VmdMorphKey* key, float value) { return static_cast<float>(key->frame) < value; });
    if (next == keys.begin()) return (*next)->weight;
    if (next == keys.end()) return keys.back()->weight;
    const auto* before = *(next - 1);
    const auto* after = *next;
    const float t = (frame - static_cast<float>(before->frame)) / static_cast<float>(after->frame - before->frame);
    return before->weight + (after->weight - before->weight) * t;
}

void calculateGlobals(const PmxModel& model, const std::vector<LocalPose>& local, std::vector<GlobalPose>& global) {
    std::vector<std::uint8_t> state(model.bones.size());
    std::function<void(std::size_t)> resolve = [&](std::size_t index) {
        if (state[index] == 2) return;
        if (state[index] == 1) { // malformed parent cycle
            global[index] = { add(model.bones[index].position, local[index].translation), local[index].rotation };
            state[index] = 2;
            return;
        }
        state[index] = 1;
        const auto parent = model.bones[index].parent;
        if (parent >= 0 && static_cast<std::size_t>(parent) < model.bones.size()) {
            resolve(static_cast<std::size_t>(parent));
            const auto bindOffset = sub(model.bones[index].position, model.bones[static_cast<std::size_t>(parent)].position);
            global[index].position = add(global[static_cast<std::size_t>(parent)].position,
                rotate(global[static_cast<std::size_t>(parent)].rotation, add(bindOffset, local[index].translation)));
            global[index].rotation = multiply(global[static_cast<std::size_t>(parent)].rotation, local[index].rotation);
        } else {
            global[index].position = add(model.bones[index].position, local[index].translation);
            global[index].rotation = local[index].rotation;
        }
        state[index] = 2;
    };
    for (std::size_t i = 0; i < model.bones.size(); ++i) resolve(i);
}

void solveIk(const PmxModel& model, std::vector<LocalPose>& local, std::vector<GlobalPose>& global) {
    for (std::size_t ikIndex = 0; ikIndex < model.bones.size(); ++ikIndex) {
        const auto& ik = model.bones[ikIndex];
        if ((ik.flags & 0x0020U) == 0 || ik.ikTarget < 0 || static_cast<std::size_t>(ik.ikTarget) >= global.size()) continue;
        const int loops = std::clamp(ik.ikLoopCount, 0, 255);
        for (int loop = 0; loop < loops; ++loop) {
            bool reached = false;
            for (const auto& link : ik.ikLinks) {
                if (link.bone < 0 || static_cast<std::size_t>(link.bone) >= global.size()) continue;
                const auto linkIndex = static_cast<std::size_t>(link.bone);
                const auto effector = static_cast<std::size_t>(ik.ikTarget);
                const auto toEffector = normalized(sub(global[effector].position, global[linkIndex].position));
                const auto toGoal = normalized(sub(global[ikIndex].position, global[linkIndex].position));
                const float cosine = std::clamp(dot(toEffector, toGoal), -1.0F, 1.0F);
                float angle = std::acos(cosine);
                if (angle < 1e-5F) { reached = true; continue; }
                angle = std::min(angle, std::max(ik.ikLimitAngle, 1e-5F));
                auto axis = normalized(cross(toEffector, toGoal));
                if (length(axis) < 1e-6F) continue;
                const auto worldDelta = axisAngle(axis, angle);
                const auto parent = model.bones[linkIndex].parent;
                const auto parentRotation = parent >= 0 && static_cast<std::size_t>(parent) < global.size()
                    ? global[static_cast<std::size_t>(parent)].rotation : Quat { 0.0F, 0.0F, 0.0F, 1.0F };
                const auto localDelta = multiply(multiply(conjugate(parentRotation), worldDelta), parentRotation);
                local[linkIndex].rotation = multiply(localDelta, local[linkIndex].rotation);
                // Knee links are conventionally constrained to their local X axis.
                if (link.limited && std::abs(link.minimum[1]) < 1e-4F && std::abs(link.maximum[1]) < 1e-4F
                                 && std::abs(link.minimum[2]) < 1e-4F && std::abs(link.maximum[2]) < 1e-4F) {
                    const float x = std::clamp(2.0F * std::atan2(local[linkIndex].rotation[0], local[linkIndex].rotation[3]),
                                               link.minimum[0], link.maximum[0]);
                    local[linkIndex].rotation = axisAngle({ 1.0F, 0.0F, 0.0F }, x);
                }
                calculateGlobals(model, local, global);
            }
            if (reached || length(sub(global[static_cast<std::size_t>(ik.ikTarget)].position, global[ikIndex].position)) < 1e-4F) break;
        }
    }
}

Float3 transformPoint(const GlobalPose& pose, const Float3& bindPosition, const Float3& value) {
    return add(rotate(pose.rotation, sub(value, bindPosition)), pose.position);
}

void skinSdef(PmxVertex& vertex, const PmxModel& model, const std::vector<GlobalPose>& global) {
    const auto first = vertex.bones[0];
    const auto second = vertex.bones[1];
    if (first < 0 || second < 0 || static_cast<std::size_t>(first) >= global.size()
        || static_cast<std::size_t>(second) >= global.size()) return;
    const float weight = std::clamp(vertex.weights[0], 0.0F, 1.0F);
    const auto halfDelta = mul(sub(vertex.sdefR0, vertex.sdefR1), 0.5F);
    const auto cr0 = add(vertex.sdefC, mul(halfDelta, 1.0F - weight));
    const auto cr1 = sub(vertex.sdefC, mul(halfDelta, weight));
    const auto rotation = slerp(global[static_cast<std::size_t>(second)].rotation,
                                global[static_cast<std::size_t>(first)].rotation, weight);
    const auto translated0 = transformPoint(global[static_cast<std::size_t>(first)],
                                             model.bones[static_cast<std::size_t>(first)].position, cr0);
    const auto translated1 = transformPoint(global[static_cast<std::size_t>(second)],
                                             model.bones[static_cast<std::size_t>(second)].position, cr1);
    vertex.position = add(rotate(rotation, sub(vertex.position, vertex.sdefC)),
                          add(mul(translated0, weight), mul(translated1, 1.0F - weight)));
    vertex.normal = normalized(rotate(rotation, vertex.normal));
}

void skinQdef(PmxVertex& vertex, const PmxModel& model, const std::vector<GlobalPose>& global) {
    Quat real {};
    Quat dual {};
    Quat pivot {};
    bool initialized = false;
    for (std::size_t influence = 0; influence < 4; ++influence) {
        const auto bone = vertex.bones[influence];
        float weight = vertex.weights[influence];
        if (bone < 0 || static_cast<std::size_t>(bone) >= global.size() || weight == 0.0F) continue;
        const auto index = static_cast<std::size_t>(bone);
        const auto rotation = global[index].rotation;
        const auto translation = sub(global[index].position,
                                     rotate(rotation, model.bones[index].position));
        auto dualPart = multiplyRaw({ translation[0], translation[1], translation[2], 0.0F }, rotation);
        for (auto& component : dualPart) component *= 0.5F;
        if (!initialized) {
            pivot = rotation;
            initialized = true;
        } else if (dot({ pivot[0], pivot[1], pivot[2] },
                       { rotation[0], rotation[1], rotation[2] }) + pivot[3] * rotation[3] < 0.0F) {
            weight = -weight;
        }
        for (std::size_t component = 0; component < 4; ++component) {
            real[component] += rotation[component] * weight;
            dual[component] += dualPart[component] * weight;
        }
    }
    const float magnitude = std::sqrt(real[0] * real[0] + real[1] * real[1]
                                    + real[2] * real[2] + real[3] * real[3]);
    if (!initialized || magnitude <= 1e-8F) return;
    for (std::size_t component = 0; component < 4; ++component) {
        real[component] /= magnitude;
        dual[component] /= magnitude;
    }
    const float projection = real[0] * dual[0] + real[1] * dual[1]
                           + real[2] * dual[2] + real[3] * dual[3];
    for (std::size_t component = 0; component < 4; ++component) dual[component] -= real[component] * projection;
    const auto translation = multiplyRaw(dual, conjugate(real));
    vertex.position = add(rotate(real, vertex.position),
                          { 2.0F * translation[0], 2.0F * translation[1], 2.0F * translation[2] });
    vertex.normal = normalized(rotate(real, vertex.normal));
}

} // namespace

MmdAnimator::MmdAnimator(const PmxModel& model) : model_(model) {}
void MmdAnimator::setMotion(const VmdMotion* motion) { motion_ = motion; }
void MmdAnimator::setPose(const VpdPose* pose) { pose_ = pose; }
void MmdAnimator::setPhysics(MmdPhysics* physics) { physics_ = physics; previousFrame_ = -1.0F; }

AnimatedModelFrame MmdAnimator::evaluate(float frame, float deltaSeconds) {
    AnimatedModelFrame result;
    result.vertices = model_.vertices;
    result.materials.reserve(model_.materials.size());
    for (const auto& material : model_.materials) {
        AnimatedModelFrame::Material animated;
        animated.diffuse = material.diffuse;
        animated.specular = material.specular;
        animated.shininess = material.shininess;
        animated.ambient = material.ambient;
        animated.edgeColor = material.edgeColor;
        animated.edgeSize = material.edgeSize;
        result.materials.push_back(animated);
    }

    std::unordered_map<std::string_view, std::vector<const VmdBoneKey*>> boneKeys;
    std::unordered_map<std::string_view, std::vector<const VmdMorphKey*>> morphKeys;
    if (motion_ != nullptr) {
        for (const auto& key : motion_->bones) boneKeys[key.name].push_back(&key);
        for (const auto& key : motion_->morphs) morphKeys[key.name].push_back(&key);
        for (auto& [name, keys] : boneKeys) std::ranges::sort(keys, {}, &VmdBoneKey::frame);
        for (auto& [name, keys] : morphKeys) std::ranges::sort(keys, {}, &VmdMorphKey::frame);
        if (!motion_->ik.empty()) {
            const auto key = std::upper_bound(motion_->ik.begin(), motion_->ik.end(), frame,
                [](float value, const VmdIkKey& item) { return value < static_cast<float>(item.frame); });
            if (key != motion_->ik.begin()) result.visible = (key - 1)->visible;
        }
    }

    std::vector<LocalPose> local(model_.bones.size());
    for (std::size_t i = 0; i < model_.bones.size(); ++i) {
        if (const auto found = boneKeys.find(model_.bones[i].name); found != boneKeys.end()) local[i] = sampleBone(found->second, frame);
    }
    if (pose_ != nullptr) {
        for (const auto& value : pose_->bones) {
            const auto bone = std::find_if(model_.bones.begin(), model_.bones.end(),
                [&](const PmxBone& item) { return item.name == value.name; });
            if (bone != model_.bones.end()) {
                const auto index = static_cast<std::size_t>(bone - model_.bones.begin());
                local[index].translation = add(local[index].translation, value.translation);
                local[index].rotation = multiply(local[index].rotation, value.rotation);
            }
        }
    }

    std::vector<float> morphWeights(model_.morphs.size());
    for (std::size_t i = 0; i < model_.morphs.size(); ++i) {
        if (const auto found = morphKeys.find(model_.morphs[i].name); found != morphKeys.end()) {
            morphWeights[i] = sampleMorph(found->second, frame);
        }
    }
    std::vector<std::uint8_t> morphStack(model_.morphs.size());
    std::function<void(std::size_t, float)> applyMorph = [&](std::size_t index, float weight) {
        if (index >= model_.morphs.size() || morphStack[index] != 0 || std::abs(weight) < 1e-8F) return;
        morphStack[index] = 1;
        const auto& morph = model_.morphs[index];
        for (const auto& offset : morph.offsets) {
            if (morph.type == 0 || morph.type == 9) applyMorph(static_cast<std::size_t>(offset.index), weight * offset.scalar);
            else if (morph.type == 1 && offset.index >= 0 && static_cast<std::size_t>(offset.index) < result.vertices.size()) {
                result.vertices[static_cast<std::size_t>(offset.index)].position = add(
                    result.vertices[static_cast<std::size_t>(offset.index)].position, mul(offset.vector3, weight));
            } else if (morph.type == 2 && offset.index >= 0 && static_cast<std::size_t>(offset.index) < local.size()) {
                const auto bone = static_cast<std::size_t>(offset.index);
                local[bone].translation = add(local[bone].translation, mul(offset.vector3, weight));
                local[bone].rotation = multiply(local[bone].rotation,
                    slerp({ 0.0F, 0.0F, 0.0F, 1.0F }, offset.vector4, weight));
            } else if (morph.type == 8) {
                const auto first = offset.index < 0 ? std::size_t { 0 } : static_cast<std::size_t>(offset.index);
                const auto last = offset.index < 0 ? result.materials.size() : std::min(first + 1, result.materials.size());
                for (std::size_t material = first; material < last; ++material) {
                    auto& destination = result.materials[material];
                    const auto apply4 = [&](Float4& value, const Float4& morphValue) {
                        for (std::size_t component = 0; component < 4; ++component) {
                            if (offset.operation == 0) value[component] *= 1.0F + (morphValue[component] - 1.0F) * weight;
                            else value[component] += morphValue[component] * weight;
                        }
                    };
                    const auto apply3 = [&](Float3& value, const Float4& morphValue) {
                        for (std::size_t component = 0; component < 3; ++component) {
                            if (offset.operation == 0) value[component] *= 1.0F + (morphValue[component] - 1.0F) * weight;
                            else value[component] += morphValue[component] * weight;
                        }
                    };
                    apply4(destination.diffuse, offset.materialVectors[0]);
                    apply3(destination.specular, offset.materialVectors[1]);
                    apply3(destination.ambient, offset.materialVectors[2]);
                    apply4(destination.edgeColor, offset.materialVectors[3]);
                    if (offset.operation == 0) {
                        destination.shininess *= 1.0F + (offset.materialVectors[1][3] - 1.0F) * weight;
                        destination.edgeSize *= 1.0F + (offset.materialVectors[2][3] - 1.0F) * weight;
                        apply4(destination.textureMultiply, offset.materialVectors[4]);
                        apply4(destination.sphereMultiply, offset.materialVectors[5]);
                        apply4(destination.toonMultiply, offset.materialVectors[6]);
                    } else {
                        destination.shininess += offset.materialVectors[1][3] * weight;
                        destination.edgeSize += offset.materialVectors[2][3] * weight;
                        apply4(destination.textureAdd, offset.materialVectors[4]);
                        apply4(destination.sphereAdd, offset.materialVectors[5]);
                        apply4(destination.toonAdd, offset.materialVectors[6]);
                    }
                }
            }
        }
        morphStack[index] = 0;
    };
    for (std::size_t i = 0; i < morphWeights.size(); ++i) applyMorph(i, morphWeights[i]);

    // Append/inherit transforms are evaluated before IK, matching PMX deform order for the common case.
    for (std::size_t i = 0; i < model_.bones.size(); ++i) {
        const auto& bone = model_.bones[i];
        if (bone.inheritParent < 0 || static_cast<std::size_t>(bone.inheritParent) >= local.size()) continue;
        const auto parent = static_cast<std::size_t>(bone.inheritParent);
        if ((bone.flags & 0x0200U) != 0) local[i].translation = add(local[i].translation, mul(local[parent].translation, bone.inheritRatio));
        if ((bone.flags & 0x0100U) != 0) local[i].rotation = multiply(local[i].rotation,
            slerp({ 0.0F, 0.0F, 0.0F, 1.0F }, local[parent].rotation, bone.inheritRatio));
    }

    std::vector<GlobalPose> global(model_.bones.size());
    calculateGlobals(model_, local, global);
    solveIk(model_, local, global);

    if (physics_ != nullptr && physics_->available()) {
        if (previousFrame_ >= 0.0F && frame < previousFrame_) physics_->reset();
        for (std::size_t bodyIndex = 0; bodyIndex < model_.rigidBodies.size(); ++bodyIndex) {
            const auto& body = model_.rigidBodies[bodyIndex];
            if (body.mode != 0 || body.bone < 0 || static_cast<std::size_t>(body.bone) >= global.size()) continue;
            const auto bone = static_cast<std::size_t>(body.bone);
            const auto offset = sub(body.position, model_.bones[bone].position);
            const auto offsetRotation = eulerRotation(body.rotation);
            PhysicsTransform value;
            value.position = add(global[bone].position, rotate(global[bone].rotation, offset));
            value.rotation = multiply(global[bone].rotation, offsetRotation);
            physics_->setKinematicTransform(bodyIndex, value);
        }
        physics_->step(deltaSeconds);
        for (std::size_t bodyIndex = 0; bodyIndex < model_.rigidBodies.size(); ++bodyIndex) {
            const auto& body = model_.rigidBodies[bodyIndex];
            if (body.mode == 0 || body.bone < 0 || static_cast<std::size_t>(body.bone) >= global.size()) continue;
            const auto bone = static_cast<std::size_t>(body.bone);
            const auto bodyPose = physics_->bodyTransform(bodyIndex);
            const auto offset = sub(body.position, model_.bones[bone].position);
            const auto boneRotation = multiply(bodyPose.rotation, conjugate(eulerRotation(body.rotation)));
            const auto bonePosition = sub(bodyPose.position, rotate(boneRotation, offset));
            const auto parent = model_.bones[bone].parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < global.size()) {
                const auto parentIndex = static_cast<std::size_t>(parent);
                local[bone].rotation = multiply(conjugate(global[parentIndex].rotation), boneRotation);
                const auto bindOffset = sub(model_.bones[bone].position, model_.bones[parentIndex].position);
                local[bone].translation = sub(rotate(conjugate(global[parentIndex].rotation),
                                                     sub(bonePosition, global[parentIndex].position)), bindOffset);
            } else {
                local[bone].rotation = boneRotation;
                local[bone].translation = sub(bonePosition, model_.bones[bone].position);
            }
            calculateGlobals(model_, local, global);
        }
    }
    previousFrame_ = frame;

    for (auto& vertex : result.vertices) {
        if (vertex.weightType == PmxWeightType::sdef) {
            skinSdef(vertex, model_, global);
            continue;
        }
        if (vertex.weightType == PmxWeightType::qdef) {
            skinQdef(vertex, model_, global);
            continue;
        }
        Float3 position {};
        Float3 normal {};
        float totalWeight = 0.0F;
        const std::size_t influenceCount = vertex.weightType == PmxWeightType::bdef1 ? 1 :
            (vertex.weightType == PmxWeightType::bdef2 || vertex.weightType == PmxWeightType::sdef ? 2 : 4);
        for (std::size_t influence = 0; influence < influenceCount; ++influence) {
            const auto bone = vertex.bones[influence];
            const float weight = vertex.weights[influence];
            if (bone < 0 || static_cast<std::size_t>(bone) >= global.size() || weight == 0.0F) continue;
            position = add(position, mul(transformPoint(global[static_cast<std::size_t>(bone)],
                model_.bones[static_cast<std::size_t>(bone)].position, vertex.position), weight));
            normal = add(normal, mul(rotate(global[static_cast<std::size_t>(bone)].rotation, vertex.normal), weight));
            totalWeight += weight;
        }
        if (totalWeight > 1e-6F) {
            vertex.position = mul(position, 1.0F / totalWeight);
            vertex.normal = normalized(normal);
        }
    }
    return result;
}

PreviewNormalization previewNormalization(const PmxModel& model) {
    if (model.vertices.empty()) return {};
    auto minimum = model.vertices.front().position;
    auto maximum = minimum;
    for (const auto& vertex : model.vertices) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
            maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
        }
    }
    PreviewNormalization result;
    result.center = { (minimum[0] + maximum[0]) * 0.5F, (minimum[1] + maximum[1]) * 0.5F,
                      (minimum[2] + maximum[2]) * 0.5F };
    result.scale = 1.8F / std::max({ maximum[0] - minimum[0], maximum[1] - minimum[1],
                                     maximum[2] - minimum[2], 0.001F });
    return result;
}

void normalizeForPreview(std::vector<PmxVertex>& vertices, const PmxModel& model) {
    if (vertices.empty() || model.vertices.empty()) return;
    const auto normalization = previewNormalization(model);
    for (auto& vertex : vertices) for (std::size_t axis = 0; axis < 3; ++axis) {
        vertex.position[axis] = (vertex.position[axis] - normalization.center[axis]) * normalization.scale;
    }
}

} // namespace dayo::core
