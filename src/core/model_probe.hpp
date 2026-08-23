#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dayo::core {

using Float2 = std::array<float, 2>;
using Float3 = std::array<float, 3>;
using Float4 = std::array<float, 4>;

struct PmxMetadata {
    float version {};
    std::string modelName;
    std::string englishName;
    std::string comment;
    std::string englishComment;
    std::int32_t vertexCount {};
    std::uint8_t textEncoding {};
    std::uint8_t additionalUvCount {};
};

enum class PmxWeightType : std::uint8_t { bdef1, bdef2, bdef4, sdef, qdef };

struct PmxVertex {
    Float3 position {};
    Float3 normal {};
    Float2 uv {};
    std::array<Float4, 4> additionalUv {};
    PmxWeightType weightType { PmxWeightType::bdef1 };
    std::array<std::int32_t, 4> bones { -1, -1, -1, -1 };
    Float4 weights { 1.0F, 0.0F, 0.0F, 0.0F };
    Float3 sdefC {};
    Float3 sdefR0 {};
    Float3 sdefR1 {};
    float edgeScale { 1.0F };
};

struct PmxMaterial {
    std::string name;
    std::string englishName;
    Float4 diffuse {};
    Float3 specular {};
    float shininess {};
    Float3 ambient {};
    std::uint8_t drawFlags {};
    Float4 edgeColor {};
    float edgeSize {};
    std::int32_t textureIndex { -1 };
    std::int32_t sphereTextureIndex { -1 };
    std::uint8_t sphereMode {};
    std::uint8_t toonMode {};
    std::int32_t toonTextureIndex { -1 };
    std::string memo;
    std::uint32_t indexCount {};
};

struct PmxIkLink {
    std::int32_t bone { -1 };
    bool limited {};
    Float3 minimum {};
    Float3 maximum {};
};

struct PmxBone {
    std::string name;
    std::string englishName;
    Float3 position {};
    std::int32_t parent { -1 };
    std::int32_t deformLayer {};
    std::uint16_t flags {};
    Float3 tailOffset {};
    std::int32_t tailBone { -1 };
    std::int32_t inheritParent { -1 };
    float inheritRatio {};
    Float3 fixedAxis {};
    Float3 localAxisX {};
    Float3 localAxisZ {};
    std::int32_t externalParentKey {};
    std::int32_t ikTarget { -1 };
    std::int32_t ikLoopCount {};
    float ikLimitAngle {};
    std::vector<PmxIkLink> ikLinks;
};

// Morph type determines which fields are populated. Keeping one compact value type
// avoids heap-owning variants in the animation hot path.
struct PmxMorphOffset {
    std::int32_t index { -1 };
    std::int32_t secondaryIndex { -1 };
    std::uint8_t operation {};
    bool local {};
    float scalar {};
    Float3 vector3 {};
    Float4 vector4 {};
    Float4 secondaryVector4 {};
    // diffuse, specular+power, ambient+edge size, edge, texture, sphere, toon, reserved
    std::array<Float4, 8> materialVectors {};
    Float3 tertiaryVector3 {};
};

struct PmxMorph {
    std::string name;
    std::string englishName;
    std::uint8_t panel {};
    std::uint8_t type {};
    std::vector<PmxMorphOffset> offsets;
};

struct PmxDisplayItem { bool bone {}; std::int32_t index { -1 }; };
struct PmxDisplayFrame {
    std::string name;
    std::string englishName;
    bool special {};
    std::vector<PmxDisplayItem> items;
};

struct PmxRigidBody {
    std::string name;
    std::string englishName;
    std::int32_t bone { -1 };
    std::uint8_t group {};
    std::uint16_t collisionMask {};
    std::uint8_t shape {};
    Float3 size {};
    Float3 position {};
    Float3 rotation {};
    float mass {};
    float linearDamping {};
    float angularDamping {};
    float restitution {};
    float friction {};
    std::uint8_t mode {};
};

struct PmxJoint {
    std::string name;
    std::string englishName;
    std::uint8_t type {};
    std::int32_t bodyA { -1 };
    std::int32_t bodyB { -1 };
    Float3 position {};
    Float3 rotation {};
    Float3 translationMinimum {};
    Float3 translationMaximum {};
    Float3 rotationMinimum {};
    Float3 rotationMaximum {};
    Float3 translationSpring {};
    Float3 rotationSpring {};
};

struct PmxSoftBodyAnchor { std::int32_t rigidBody { -1 }; std::int32_t vertex { -1 }; bool nearMode {}; };
struct PmxSoftBody {
    std::string name;
    std::string englishName;
    std::uint8_t shape {};
    std::int32_t material { -1 };
    std::uint8_t group {};
    std::uint16_t collisionMask {};
    std::uint8_t flags {};
    std::int32_t bendingLinkDistance {};
    std::int32_t clusterCount {};
    float totalMass {};
    float collisionMargin {};
    std::int32_t aeroModel {};
    std::array<float, 12> config {};
    std::array<float, 6> cluster {};
    std::array<std::int32_t, 4> iteration {};
    std::array<float, 3> materialConfig {};
    std::vector<PmxSoftBodyAnchor> anchors;
    std::vector<std::int32_t> pinnedVertices;
};

struct PmxModel {
    PmxMetadata metadata;
    std::filesystem::path sourcePath;
    std::vector<PmxVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::filesystem::path> textures;
    std::vector<PmxMaterial> materials;
    std::vector<PmxBone> bones;
    std::vector<PmxMorph> morphs;
    std::vector<PmxDisplayFrame> displayFrames;
    std::vector<PmxRigidBody> rigidBodies;
    std::vector<PmxJoint> joints;
    std::vector<PmxSoftBody> softBodies;
};

struct PmxMesh { PmxMetadata metadata; std::vector<PmxVertex> vertices; std::vector<std::uint32_t> indices; };

[[nodiscard]] PmxMetadata probePmx(const std::filesystem::path& path);
[[nodiscard]] PmxModel loadPmxModel(const std::filesystem::path& path);
[[nodiscard]] PmxMesh loadPmxMesh(const std::filesystem::path& path);

} // namespace dayo::core
