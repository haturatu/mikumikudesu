#include "core/skirt_collision.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace dayo::core {
namespace {
std::string lower(std::string value) {
    for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
bool skirtName(const std::string& name) {
    const auto text = lower(name);
    return text.find("skirt") != std::string::npos || text.find("スカート") != std::string::npos ||
           text.find("裙") != std::string::npos;
}
bool legName(const std::string& name) {
    const auto text = lower(name);
    if (text.find("足首") != std::string::npos || text.find("ankle") != std::string::npos)
        return false;
    return text == "左足" || text == "右足" || text == "左ひざ" || text == "右ひざ" ||
           text.find("thigh") != std::string::npos || text.find("knee") != std::string::npos ||
           text.find("leg") != std::string::npos;
}
Float3 rotate(const Float4& q, Float3 p) {
    const Float3 t{2 * (q[1] * p[2] - q[2] * p[1]), 2 * (q[2] * p[0] - q[0] * p[2]), 2 * (q[0] * p[1] - q[1] * p[0])};
    return {p[0] + q[3] * t[0] + q[1] * t[2] - q[2] * t[1], p[1] + q[3] * t[1] + q[2] * t[0] - q[0] * t[2],
            p[2] + q[3] * t[2] + q[0] * t[1] - q[1] * t[0]};
}
Float4 multiply(const Float4& a, const Float4& b) {
    return {
        a[3] * b[0] + b[3] * a[0] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] + b[3] * a[1] + a[2] * b[0] - a[0] * b[2],
        a[3] * b[2] + b[3] * a[2] + a[0] * b[1] - a[1] * b[0], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Float4 euler(Float3 angles) {
    Float4 q{0, 0, 0, 1};
    for (int axis = 2; axis >= 0; --axis) {
        Float4 r{0, 0, 0, std::cos(angles[static_cast<std::size_t>(axis)] / 2)};
        r[static_cast<std::size_t>(axis)] = std::sin(angles[static_cast<std::size_t>(axis)] / 2);
        q = multiply(q, r);
    }
    return q;
}
bool collides(const PmxRigidBody& a, const PmxRigidBody& b) {
    return (a.collisionMask & (1U << std::min<unsigned>(b.group, 15))) != 0 &&
           (b.collisionMask & (1U << std::min<unsigned>(a.group, 15))) != 0;
}
} // namespace

SkirtCollisionTopology identifySkirtCollision(const PmxModel& model) {
    SkirtCollisionTopology result;
    std::vector<bool> skirtBones(model.bones.size());
    for (std::size_t i = 0; i < model.rigidBodies.size(); ++i) {
        const auto& body = model.rigidBodies[i];
        const bool validBone = body.bone >= 0 && static_cast<std::size_t>(body.bone) < model.bones.size();
        if (body.physicsEnabled && body.mode != 0 &&
            (skirtName(body.name) || skirtName(body.englishName) ||
             (validBone && skirtName(model.bones[static_cast<std::size_t>(body.bone)].name)))) {
            result.skirtBodies.push_back(i);
            if (validBone)
                skirtBones[static_cast<std::size_t>(body.bone)] = true;
        }
    }
    for (std::size_t i = 0; i < model.rigidBodies.size(); ++i) {
        const auto& body = model.rigidBodies[i];
        const bool namedLeg = legName(body.name) || legName(body.englishName) ||
                              (body.bone >= 0 && static_cast<std::size_t>(body.bone) < model.bones.size() &&
                               legName(model.bones[static_cast<std::size_t>(body.bone)].name));
        if (!body.physicsEnabled || body.mode != 0 || body.shape != 2 || !namedLeg || !std::isfinite(body.size[0]) ||
            !std::isfinite(body.size[1]) || std::abs(body.size[0]) < .001F || std::abs(body.size[1]) < .001F)
            continue;
        if (std::ranges::any_of(result.skirtBodies,
                                [&](auto skirt) { return collides(body, model.rigidBodies[skirt]); }))
            result.legBodies.push_back(i);
    }
    std::vector<float> influence(model.vertices.size());
    for (std::size_t i = 0; i < model.vertices.size(); ++i) {
        const auto& v = model.vertices[i];
        for (std::size_t w = 0; w < 4; ++w)
            if (v.bones[w] >= 0 && static_cast<std::size_t>(v.bones[w]) < skirtBones.size() &&
                skirtBones[static_cast<std::size_t>(v.bones[w])] && std::isfinite(v.weights[w]))
                influence[i] += std::max(v.weights[w], 0.0F);
    }
    std::vector<std::uint32_t> lookup(model.vertices.size(), std::numeric_limits<std::uint32_t>::max());
    std::vector<std::vector<std::uint32_t>> adjacency;
    const bool hasNamedMaterial = std::ranges::any_of(model.materials, [](const auto& material) {
        return skirtName(material.name) || skirtName(material.englishName);
    });
    std::size_t first = 0;
    for (const auto& material : model.materials) {
        const auto end = first + std::min<std::size_t>(material.indexCount, model.indices.size() - first);
        std::size_t weighted = 0;
        for (auto i = first; i < end; ++i)
            if (model.indices[i] < influence.size() && influence[model.indices[i]] > .2F)
                ++weighted;
        const bool selected = skirtName(material.name) || skirtName(material.englishName) ||
                              (!hasNamedMaterial && end > first &&
                               static_cast<double>(weighted) / static_cast<double>(end - first) > .15);
        if (selected) {
            for (auto i = first; i + 2 < end; i += 3) {
                std::array<std::uint32_t, 4> triangle{};
                bool valid = true;
                for (std::size_t j = 0; j < 3; ++j)
                    valid = valid && model.indices[i + j] < lookup.size();
                if (!valid)
                    continue;
                for (std::size_t j = 0; j < 3; ++j) {
                    const auto vertex = model.indices[i + j];
                    if (lookup[vertex] == std::numeric_limits<std::uint32_t>::max()) {
                        lookup[vertex] = static_cast<std::uint32_t>(result.vertices.size());
                        SkirtVertex entry;
                        entry.vertex = vertex;
                        entry.mobility = std::clamp(.25F + .75F * influence[vertex], .25F, 1.0F);
                        entry.capsuleCount = static_cast<std::uint32_t>(result.legBodies.size());
                        result.vertices.push_back(entry);
                        adjacency.emplace_back();
                    }
                    triangle[j] = lookup[vertex];
                }
                const auto index = static_cast<std::uint32_t>(result.triangles.size());
                result.triangles.push_back(triangle);
                for (std::size_t j = 0; j < 3; ++j)
                    adjacency[triangle[j]].push_back(index);
            }
        }
        first = end;
    }
    for (std::size_t i = 0; i < result.vertices.size(); ++i) {
        auto& v = result.vertices[i];
        v.firstTriangle = static_cast<std::uint32_t>(result.adjacency.size());
        v.triangleCount = static_cast<std::uint32_t>(adjacency[i].size());
        result.adjacency.insert(result.adjacency.end(), adjacency[i].begin(), adjacency[i].end());
    }
    return result;
}

std::vector<SkirtCapsule> makeSkirtCapsules(const PmxModel& model, const SkirtCollisionTopology& topology,
                                            const MmdPhysics& physics,
                                            std::span<const AnimatedModelFrame::BoneTransform> bones,
                                            PreviewNormalization normalization,
                                            std::span<const mmd::PhysicsTransform> previous) {
    std::vector<SkirtCapsule> result;
    Float4 torsoRotation{0, 0, 0, 1};
    float torsoX = 0;
    for (std::size_t b = 0; b < model.bones.size(); ++b) {
        if (model.bones[b].name == "下半身" || lower(model.bones[b].englishName) == "lower body") {
            torsoX = model.bones[b].position[0];
            if (b < bones.size())
                torsoRotation = bones[b].rotation;
            break;
        }
    }
    for (std::size_t i = 0; i < topology.legBodies.size(); ++i) {
        const auto index = topology.legBodies[i];
        const auto& source = model.rigidBodies[index];
        mmd::PhysicsTransform pose{source.position, euler(source.rotation)};
        if (physics.available())
            pose = physics.bodyTransform(index);
        else if (source.bone >= 0 && static_cast<std::size_t>(source.bone) < bones.size()) {
            const auto& bone = bones[static_cast<std::size_t>(source.bone)];
            pose.position = rotate(bone.rotation, source.position);
            for (std::size_t axis = 0; axis < 3; ++axis)
                pose.position[axis] += bone.translation[axis];
            pose.rotation = multiply(bone.rotation, pose.rotation);
        }
        const auto endpoints = [&](const mmd::PhysicsTransform& value) {
            std::array<Float4, 2> ends{};
            const auto axis = rotate(value.rotation, {0, std::abs(source.size[1]) * .5F, 0});
            for (std::size_t c = 0; c < 3; ++c) {
                ends[0][c] = (value.position[c] - axis[c] - normalization.center[c]) * normalization.scale;
                ends[1][c] = (value.position[c] + axis[c] - normalization.center[c]) * normalization.scale;
            }
            ends[0][3] = std::abs(source.size[0]) * normalization.scale;
            ends[1][3] = .03F * normalization.scale;
            return ends;
        };
        const auto old = i < previous.size() ? previous[i] : pose;
        const auto currentEnds = endpoints(pose);
        auto previousEnds = endpoints(old);
        const float side = source.position[0] < torsoX ? -1.0F : 1.0F;
        auto escape = rotate(torsoRotation, {side * .25F, 0, 1});
        const auto axis = rotate(pose.rotation, {0, 1, 0});
        float axial = 0;
        for (std::size_t c = 0; c < 3; ++c)
            axial += escape[c] * axis[c];
        for (std::size_t c = 0; c < 3; ++c)
            escape[c] -= axis[c] * axial;
        float l1 = std::abs(escape[0]) + std::abs(escape[1]) + std::abs(escape[2]);
        if (l1 < 1e-5F) {
            escape = std::abs(axis[1]) < .9F ? Float3{-axis[2], 0, axis[0]} : Float3{0, axis[2], -axis[1]};
            l1 = std::abs(escape[0]) + std::abs(escape[1]) + std::abs(escape[2]);
        }
        for (auto& c : escape)
            c /= l1;
        float ex = escape[0], ey = escape[1];
        if (escape[2] < 0) {
            ex = (1 - std::abs(escape[1])) * (escape[0] < 0 ? -1 : 1);
            ey = (1 - std::abs(escape[0])) * (escape[1] < 0 ? -1 : 1);
        }
        // The endpoint history needs only xyz. Its spare w components hold
        // an octahedral normal for coherent recovery of a trapped panel.
        previousEnds[0][3] = ex;
        previousEnds[1][3] = ey;
        auto inverse = old.rotation;
        for (std::size_t c = 0; c < 3; ++c)
            inverse[c] = -inverse[c];
        result.push_back(
            {currentEnds[0], currentEnds[1], previousEnds[0], previousEnds[1], multiply(pose.rotation, inverse)});
    }
    return result;
}

SkirtCollisionStats skirtRigidStats(const SkirtCollisionTopology& topology, const MmdPhysics& physics) {
    SkirtCollisionStats result;
    for (const auto& contact : physics.stepStats().contacts) {
        const auto contains = [](const auto& indices, int body) {
            return body >= 0 && std::ranges::find(indices, static_cast<std::size_t>(body)) != indices.end();
        };
        if ((contains(topology.legBodies, contact.bodyA) && contains(topology.skirtBodies, contact.bodyB)) ||
            (contains(topology.legBodies, contact.bodyB) && contains(topology.skirtBodies, contact.bodyA))) {
            ++result.rigidContactCount;
            result.maxRigidPenetration = std::max(result.maxRigidPenetration, contact.penetration);
        }
    }
    return result;
}
} // namespace dayo::core
