#pragma once

#include "core/effect.hpp"
#include "core/fx/fx_expr.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dayo::core::fx {

enum class MaterialFieldType : std::uint8_t { floatingPoint, signedInteger };
enum class MaterialTextureDimension : std::uint8_t { twoD, threeD };

struct MaterialFieldSchema {
    std::string name;
    MaterialFieldType type{MaterialFieldType::floatingPoint};
    std::uint32_t components{1};
};

struct MaterialTextureSchema {
    std::string name;
    MaterialTextureDimension dimension{MaterialTextureDimension::twoD};
    std::uint32_t index{};
    bool mipmapped{};
};

struct MaterialEnumValue {
    std::string name;
    std::int32_t value{};
};

struct MaterialEnumSchema {
    std::string field;
    std::vector<MaterialEnumValue> values;
};

struct MaterialValueExpression {
    std::string field;
    std::string source;
    std::vector<FxExpr> components;
};

struct MaterialTextureAssignment {
    std::string field;
    MaterialTextureDimension dimension{MaterialTextureDimension::twoD};
    std::uint32_t index{};
    bool mipmapped{};
    std::string path;
    std::filesystem::path baseDirectory;
};

struct MaterialAnnotation {
    std::vector<MaterialValueExpression> values;
    std::vector<MaterialTextureAssignment> textures;
};

// Ordered schema for an upstream MatDesc template. Template defaults remain
// concrete (upstream disallows expressions in templates); a defaultFile is a
// separate annotation whose expressions and texture paths are retained for
// invocation-time evaluation and resource linking.
struct MaterialTemplateSchema {
    std::string name;
    std::vector<MaterialFieldSchema> fields;
    std::vector<MaterialTextureSchema> textures;
    std::vector<MaterialEnumSchema> enums;
    MaterialParameterBlock defaults;
    std::vector<MaterialTextureAssignment> templateTextureAssignments;
    MaterialAnnotation defaultFileAnnotation;
    std::filesystem::path templateTextureBaseDirectory;
    std::filesystem::path defaultFileBaseDirectory;
    std::string sourceText;
    std::string defaultFileSourceText;
};

[[nodiscard]] MaterialTemplateSchema parseMaterialTemplateSchema(std::string_view source, std::string name = {});
[[nodiscard]] MaterialTemplateSchema loadMaterialTemplateSchema(const std::filesystem::path& path,
                                                                std::string name = {},
                                                                std::filesystem::path textureBaseDirectory = {});
[[nodiscard]] MaterialAnnotation parseMaterialAnnotation(const MaterialTemplateSchema& schema, std::string_view source,
                                                         std::filesystem::path baseDirectory = {});
void applyMaterialDefaultFile(MaterialTemplateSchema& schema, std::string_view source,
                              std::filesystem::path baseDirectory = {});
void loadMaterialDefaultFile(MaterialTemplateSchema& schema, const std::filesystem::path& path);

// Linker-only material layer built on top of MaterialParameterBlock.
// No GPU work happens here: this layer folds resource aliases into
// canonical IDs, deduplicates textures, and assigns deterministic _R slots.

// Canonical texture identity: path + format + colorspace + mip policy.
struct MaterialTextureDesc {
    std::string path;
    std::string format;
    std::string colorspace;
    std::string mipPolicy;
};

struct MaterialTextureKey {
    std::string path;
    std::string format;
    std::string colorspace;
    std::string mipPolicy;

    bool operator==(const MaterialTextureKey& other) const noexcept {
        return path == other.path && format == other.format && colorspace == other.colorspace &&
               mipPolicy == other.mipPolicy;
    }
};

struct MaterialTextureKeyHash {
    [[nodiscard]] std::size_t operator()(const MaterialTextureKey& key) const noexcept;
};

[[nodiscard]] std::string normalizeTexturePath(std::string_view path);
[[nodiscard]] std::string normalizeTextureToken(std::string_view token) noexcept;
[[nodiscard]] MaterialTextureKey makeTextureKey(const MaterialTextureDesc& desc);
[[nodiscard]] std::string textureKeyString(const MaterialTextureKey& key);

// One declared resource. `ref` is an explicit alias to another id.
// `shared` puts the id in the cross-material shared namespace.
// `shareTags` merges every decl with the identical sorted tag set.
struct MaterialResourceDecl {
    std::string id;
    std::string ref;
    bool shared{};
    std::vector<std::string> shareTags;
    MaterialTextureDesc texture{};
    bool hasTexture{};
};

struct MaterialTemplate {
    std::string name;
    MaterialParameterBlock defaults;
    std::vector<MaterialResourceDecl> resources;
};

struct MaterialInstance {
    std::string templateName;
    MaterialParameterBlock overrides;
    std::vector<MaterialResourceDecl> extraResources;
};

struct MaterialGpuLayout {
    // Sorted canonical ids, e.g. {"shared:albedo", "tag:base", "local"}.
    std::vector<std::string> canonicalResources;
    // Deduplicated textures sorted by canonical key string.
    std::vector<MaterialTextureDesc> uniqueTextures;
    // Local id -> canonical id.
    std::unordered_map<std::string, std::string> localToCanonical;
    // Canonical id -> deterministic slot ("_R0", "_R1", ...).
    std::unordered_map<std::string, std::string> canonicalToSlot;

    [[nodiscard]] std::string slotForLocal(std::string_view localId) const;
    [[nodiscard]] std::string slotForCanonical(std::string_view canonical) const;
    [[nodiscard]] bool hasLocal(std::string_view localId) const noexcept;
};

struct MaterialBindingPlan {
    MaterialGpuLayout layout;
    // Template defaults overlaid with instance overrides.
    MaterialParameterBlock resolvedParameters;
    // Values in the exact HLSL StructuredBuffer<FooValue> field declaration order.
    struct ResolvedField {
        MaterialFieldSchema schema;
        MaterialValue value;
    };
    std::vector<ResolvedField> orderedValues;

    [[nodiscard]] const std::string* slotFor(std::string_view localId) const noexcept;
};

// Alias folding rules (priority: ref > shareTags > shared > concrete):
// - ref="B"        -> canonical(resolve(B)); missing target keeps "B".
// - shareTags=[..] -> canonical("tag:" + sorted-normalized-tags joined by "+").
// - shared=true    -> canonical("shared:" + trimmed id).
// - otherwise      -> canonical(trimmed id).
// Cycles resolve to the lexicographically smallest id in the cycle.
[[nodiscard]] std::string resolveCanonicalResourceId(std::string_view id,
                                                     const std::unordered_map<std::string, MaterialResourceDecl>& byId);

[[nodiscard]] MaterialGpuLayout linkMaterialLayout(const MaterialTemplate& templ,
                                                   const MaterialInstance* instance = nullptr);
[[nodiscard]] MaterialBindingPlan linkMaterial(const MaterialTemplate& templ,
                                               const MaterialInstance* instance = nullptr);
[[nodiscard]] MaterialBindingPlan linkMaterial(const MaterialTemplateSchema& schema,
                                               const MaterialInstance* instance = nullptr);

} // namespace dayo::core::fx
