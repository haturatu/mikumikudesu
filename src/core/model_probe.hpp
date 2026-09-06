#pragma once

#include <mmd/pmx.hpp>

// Transitional compatibility facade. New application code should include
// <mmd/pmx.hpp> and use mmd::pmx directly.
namespace dayo::core {
using mmd::Float2;
using mmd::Float3;
using mmd::Float4;
using mmd::PmxBone;
using mmd::PmxDisplayFrame;
using mmd::PmxDisplayItem;
using mmd::PmxIkLink;
using mmd::PmxJoint;
using mmd::PmxMaterial;
using mmd::PmxMesh;
using mmd::PmxMetadata;
using mmd::PmxModel;
using mmd::PmxMorph;
using mmd::PmxMorphOffset;
using mmd::PmxRigidBody;
using mmd::PmxSoftBody;
using mmd::PmxSoftBodyAnchor;
using mmd::PmxVertex;
using mmd::PmxWeightType;

[[nodiscard]] inline PmxMetadata probePmx(const std::filesystem::path& path) { return mmd::pmx::probe(path); }
[[nodiscard]] inline PmxModel loadPmxModel(const std::filesystem::path& path) { return mmd::pmx::load(path); }
[[nodiscard]] inline PmxMesh loadPmxMesh(const std::filesystem::path& path) { return mmd::pmx::loadMesh(path); }
} // namespace dayo::core
