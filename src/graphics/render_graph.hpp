#pragma once

#include "graphics/resource.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dayo::graphics {

using RenderGraphResource = std::uint32_t;
using RenderGraphPass = std::uint32_t;

enum class RenderResourceState {
    undefined,
    vertexInput,
    shaderRead,
    colorAttachment,
    depthAttachment,
    transferSource,
    present,
};

struct RenderGraphUse {
    RenderGraphResource resource{};
    RenderResourceState state{RenderResourceState::undefined};
    bool write{};
};

struct RenderGraphBarrier {
    RenderGraphResource resource{};
    RenderResourceState before{RenderResourceState::undefined};
    RenderResourceState after{RenderResourceState::undefined};
};

struct RenderGraphResourceLifetime {
    std::uint32_t firstPass{};
    std::uint32_t lastPass{};
};

struct RenderGraphPassPlan {
    std::string name;
    std::vector<RenderGraphPass> dependencies;
    std::vector<RenderGraphBarrier> barriers;
};

class RenderGraphLite {
  public:
    [[nodiscard]] RenderGraphResource createResource(std::string name) {
        resources_.push_back({std::move(name), {}});
        return static_cast<RenderGraphResource>(resources_.size() - 1U);
    }

    [[nodiscard]] RenderGraphPass addPass(std::string name, std::span<const RenderGraphUse> uses) {
        passes_.push_back({std::move(name), std::vector<RenderGraphUse>(uses.begin(), uses.end())});
        return static_cast<RenderGraphPass>(passes_.size() - 1U);
    }

    [[nodiscard]] RenderGraphPass addPass(std::string name, std::initializer_list<RenderGraphUse> uses) {
        return addPass(std::move(name), std::span<const RenderGraphUse>(uses.begin(), uses.size()));
    }

    [[nodiscard]] std::vector<RenderGraphPassPlan> compile() const {
        struct Tracking {
            RenderGraphPass lastWriter{invalidPass};
            std::vector<RenderGraphPass> readers;
            RenderResourceState state{RenderResourceState::undefined};
        };
        std::vector<Tracking> tracking(resources_.size());
        std::vector<RenderGraphPassPlan> result;
        result.reserve(passes_.size());
        auto addDependency = [](std::vector<RenderGraphPass>& dependencies, RenderGraphPass dependency,
                                RenderGraphPass currentPass) {
            if (dependency != invalidPass && dependency != currentPass &&
                std::find(dependencies.begin(), dependencies.end(), dependency) == dependencies.end())
                dependencies.push_back(dependency);
        };

        for (std::uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            const auto& pass = passes_[passIndex];
            RenderGraphPassPlan plan{pass.name, {}, {}};
            for (const auto& use : pass.uses) {
                if (use.resource >= resources_.size())
                    throw std::invalid_argument("render graph use references an unknown resource");
                auto& resource = tracking[use.resource];
                if (use.write) {
                    addDependency(plan.dependencies, resource.lastWriter, passIndex);
                    for (const auto reader : resource.readers)
                        addDependency(plan.dependencies, reader, passIndex);
                    resource.readers.clear();
                    resource.lastWriter = passIndex;
                } else {
                    addDependency(plan.dependencies, resource.lastWriter, passIndex);
                    resource.readers.push_back(passIndex);
                }
                if (resource.state != use.state) {
                    plan.barriers.push_back({use.resource, resource.state, use.state});
                    resource.state = use.state;
                }
            }
            result.push_back(std::move(plan));
        }
        return result;
    }

    [[nodiscard]] std::vector<RenderGraphResourceLifetime> lifetimes() const {
        std::vector<RenderGraphResourceLifetime> result(resources_.size(), {invalidPass, invalidPass});
        for (std::uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            for (const auto& use : passes_[passIndex].uses) {
                if (use.resource >= resources_.size())
                    throw std::invalid_argument("render graph use references an unknown resource");
                auto& lifetime = result[use.resource];
                lifetime.firstPass = std::min(lifetime.firstPass, passIndex);
                lifetime.lastPass = passIndex;
            }
        }
        return result;
    }

    static constexpr RenderGraphPass invalidPass = UINT32_MAX;

  private:
    struct Resource {
        std::string name;
        RenderGraphResourceLifetime lifetime;
    };
    struct Pass {
        std::string name;
        std::vector<RenderGraphUse> uses;
    };

    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
};

// Typed render graph built on ResourceUsage/ResourceLifetime. RenderGraphLite
// above is preserved unchanged for the Preview backend; this class adds the
// 20-usage barrier model plus per-resource descriptors for alias analysis.
using TypedResource = std::uint32_t;
using TypedPass = std::uint32_t;

struct TypedUse {
    TypedResource resource{};
    ResourceUsage usage{ResourceUsage::sampledRead};
};

struct TypedBarrier {
    TypedResource resource{};
    ResourceUsage before{ResourceUsage::none};
    ResourceUsage after{ResourceUsage::none};
};

struct TypedLifetime {
    std::uint32_t firstPass{};
    std::uint32_t lastPass{};
};

struct TypedPassPlan {
    std::string name;
    std::vector<TypedPass> dependencies;
    std::vector<TypedBarrier> barriers;
};

// Two pass ranges overlap unless one ends strictly before the other starts.
// Unused resources (invalidPass on both ends) overlap nothing and may share.
[[nodiscard]] inline bool lifetimesOverlap(const TypedLifetime& left, const TypedLifetime& right) noexcept {
    constexpr std::uint32_t invalid = UINT32_MAX;
    const bool leftUnused = left.firstPass == invalid && left.lastPass == invalid;
    const bool rightUnused = right.firstPass == invalid && right.lastPass == invalid;
    if (leftUnused || rightUnused)
        return false;
    return left.firstPass <= right.lastPass && right.firstPass <= left.lastPass;
}

class RenderGraph {
  public:
    static constexpr TypedPass invalidPass = UINT32_MAX;

    [[nodiscard]] TypedResource createResource(std::string name) {
        resources_.push_back(Resource{std::move(name), std::nullopt, std::nullopt});
        return static_cast<TypedResource>(resources_.size() - 1U);
    }

    [[nodiscard]] TypedResource createResource(std::string name, const TextureResourceDesc& desc) {
        resources_.push_back(Resource{std::move(name), desc, std::nullopt});
        return static_cast<TypedResource>(resources_.size() - 1U);
    }

    [[nodiscard]] TypedResource createBufferResource(std::string name, const BufferResourceDesc& desc) {
        resources_.push_back(Resource{std::move(name), std::nullopt, desc});
        return static_cast<TypedResource>(resources_.size() - 1U);
    }

    [[nodiscard]] TypedPass addPass(std::string name, std::span<const TypedUse> uses) {
        passes_.push_back(Pass{std::move(name), std::vector<TypedUse>(uses.begin(), uses.end())});
        return static_cast<TypedPass>(passes_.size() - 1U);
    }

    [[nodiscard]] TypedPass addPass(std::string name, std::initializer_list<TypedUse> uses) {
        return addPass(std::move(name), std::span<const TypedUse>(uses.begin(), uses.size()));
    }

    [[nodiscard]] std::size_t resourceCount() const noexcept {
        return resources_.size();
    }

    [[nodiscard]] std::size_t passCount() const noexcept {
        return passes_.size();
    }

    [[nodiscard]] const TextureResourceDesc* resourceDesc(TypedResource resource) const noexcept {
        if (resource >= resources_.size())
            return nullptr;
        const auto& desc = resources_[resource].desc;
        return desc.has_value() ? &desc.value() : nullptr;
    }

    [[nodiscard]] const BufferResourceDesc* resourceBufferDesc(TypedResource resource) const noexcept {
        if (resource >= resources_.size())
            return nullptr;
        const auto& desc = resources_[resource].bufferDesc;
        return desc.has_value() ? &desc.value() : nullptr;
    }

    // Lifetime-only alias query: transient resources may share an allocation
    // candidate, while persistent history (PreviousFrame/BDPT accumulation,
    // particles/history) may not. Descriptorless resources cannot be paired
    // with a typed descriptor without backend requirements.
    [[nodiscard]] bool mayAliasByLifetime(TypedResource left, TypedResource right) const noexcept {
        if (left >= resources_.size() || right >= resources_.size())
            return false;
        const auto& leftRes = resources_[left];
        const auto& rightRes = resources_[right];
        if (leftRes.desc.has_value() && rightRes.desc.has_value())
            return graphics::mayAliasByLifetime(leftRes.desc.value(), rightRes.desc.value());
        if (leftRes.bufferDesc.has_value() && rightRes.bufferDesc.has_value())
            return graphics::mayAliasByLifetime(leftRes.bufferDesc.value(), rightRes.bufferDesc.value());
        if (!leftRes.desc.has_value() && !leftRes.bufferDesc.has_value() && !rightRes.desc.has_value() &&
            !rightRes.bufferDesc.has_value())
            return true;
        // Texture/buffer pairs cannot share an alias group without backend
        // memory-requirement validation, even when both are transient.
        return false;
    }

    // Compatibility name for CPU callers. Physical Vulkan aliasing must use
    // backend memory requirements in addition to this lifetime result.
    [[nodiscard]] bool canAlias(TypedResource left, TypedResource right) const noexcept {
        return mayAliasByLifetime(left, right);
    }

    // Overlap-aware alias query: even two transient resources may only alias
    // when their pass lifetimes are disjoint (e.g. A: pass 0-2, B: pass 3-5).
    // Overlapping lifetimes or any persistent endpoint forbids aliasing.
    [[nodiscard]] bool canAliasWithLifetimes(TypedResource left, TypedResource right) const {
        if (!mayAliasByLifetime(left, right))
            return false;
        const auto lives = lifetimes();
        if (left >= lives.size() || right >= lives.size())
            return false;
        const auto isUnused = [](const TypedLifetime& lifetime) {
            return lifetime.firstPass == invalidPass && lifetime.lastPass == invalidPass;
        };
        if (isUnused(lives[left]) || isUnused(lives[right]))
            return false;
        return !lifetimesOverlap(lives[left], lives[right]);
    }

    // Greedy alias-group assignment for VMA: resources sharing a group id may
    // be suballocated from one physical allocation. Overlapping or persistent
    // resources always land in distinct groups.
    [[nodiscard]] std::vector<std::uint32_t> computeAliasGroups() const {
        const auto lives = lifetimes();
        constexpr auto noAliasGroup = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint32_t> groups(resources_.size(), noAliasGroup);
        std::uint32_t nextGroup = 0;
        for (std::size_t i = 0; i < resources_.size(); ++i) {
            if (lives[i].firstPass == invalidPass && lives[i].lastPass == invalidPass)
                continue;
            std::uint32_t assigned = nextGroup;
            for (std::uint32_t candidate = 0; candidate < nextGroup; ++candidate) {
                bool fits = true;
                for (std::size_t j = 0; j < i; ++j) {
                    if (groups[j] != candidate)
                        continue;
                    if (!mayAliasByLifetime(static_cast<TypedResource>(i), static_cast<TypedResource>(j)) ||
                        lifetimesOverlap(lives[i], lives[j])) {
                        fits = false;
                        break;
                    }
                }
                if (fits) {
                    assigned = candidate;
                    break;
                }
            }
            groups[i] = assigned;
            if (assigned == nextGroup)
                ++nextGroup;
        }
        return groups;
    }

    [[nodiscard]] std::vector<TypedPassPlan> compile() const {
        struct Tracking {
            TypedPass lastWriter{invalidPass};
            std::vector<TypedPass> readers;
            ResourceUsage state{ResourceUsage::none};
        };
        std::vector<Tracking> tracking(resources_.size());
        std::vector<TypedPassPlan> result;
        result.reserve(passes_.size());
        auto addDependency = [](std::vector<TypedPass>& dependencies, TypedPass dependency, TypedPass currentPass) {
            if (dependency != invalidPass && dependency != currentPass &&
                std::find(dependencies.begin(), dependencies.end(), dependency) == dependencies.end())
                dependencies.push_back(dependency);
        };

        for (std::uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            const auto& pass = passes_[passIndex];
            TypedPassPlan plan{pass.name, {}, {}};
            for (const auto& use : pass.uses) {
                if (use.resource >= resources_.size())
                    throw std::invalid_argument("render graph use references an unknown resource");
                auto& resource = tracking[use.resource];
                const bool write = isWriteUsage(use.usage);
                if (write) {
                    addDependency(plan.dependencies, resource.lastWriter, passIndex);
                    for (const auto reader : resource.readers)
                        addDependency(plan.dependencies, reader, passIndex);
                    resource.readers.clear();
                    resource.lastWriter = passIndex;
                } else {
                    addDependency(plan.dependencies, resource.lastWriter, passIndex);
                    resource.readers.push_back(passIndex);
                }
                if (resource.state != use.usage) {
                    plan.barriers.push_back({use.resource, resource.state, use.usage});
                    resource.state = use.usage;
                }
            }
            result.push_back(std::move(plan));
        }
        return result;
    }

    [[nodiscard]] std::vector<TypedLifetime> lifetimes() const {
        std::vector<TypedLifetime> result(resources_.size(), {invalidPass, invalidPass});
        for (std::uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            for (const auto& use : passes_[passIndex].uses) {
                if (use.resource >= resources_.size())
                    throw std::invalid_argument("render graph use references an unknown resource");
                auto& lifetime = result[use.resource];
                lifetime.firstPass = std::min(lifetime.firstPass, passIndex);
                lifetime.lastPass = passIndex;
            }
        }
        return result;
    }

  private:
    [[nodiscard]] ResourceLifetime lifetimeOf(TypedResource resource) const noexcept {
        const auto& res = resources_[resource];
        if (res.desc.has_value())
            return res.desc->lifetime;
        if (res.bufferDesc.has_value())
            return res.bufferDesc->lifetime;
        return ResourceLifetime::transient;
    }

    struct Resource {
        std::string name;
        std::optional<TextureResourceDesc> desc;
        std::optional<BufferResourceDesc> bufferDesc;
    };
    struct Pass {
        std::string name;
        std::vector<TypedUse> uses;
    };

    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
};

} // namespace dayo::graphics
