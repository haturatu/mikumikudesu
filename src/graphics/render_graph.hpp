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

class RenderGraph {
  public:
    static constexpr TypedPass invalidPass = UINT32_MAX;

    [[nodiscard]] TypedResource createResource(std::string name) {
        resources_.push_back(Resource{std::move(name), std::nullopt});
        return static_cast<TypedResource>(resources_.size() - 1U);
    }

    [[nodiscard]] TypedResource createResource(std::string name, const TextureResourceDesc& desc) {
        resources_.push_back(Resource{std::move(name), desc});
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

    // Alias query backed by ResourceLifetime: transient resources may share
    // physical memory, persistent history (PreviousFrame/BDPT accumulation,
    // particles/history) may not. Resources without a descriptor are treated
    // as transient scratch.
    [[nodiscard]] bool canAlias(TypedResource left, TypedResource right) const noexcept {
        if (left >= resources_.size() || right >= resources_.size())
            return false;
        const auto& leftDesc = resources_[left].desc;
        const auto& rightDesc = resources_[right].desc;
        if (!leftDesc.has_value() || !rightDesc.has_value())
            return true;
        return graphics::canAlias(leftDesc.value(), rightDesc.value());
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
    struct Resource {
        std::string name;
        std::optional<TextureResourceDesc> desc;
    };
    struct Pass {
        std::string name;
        std::vector<TypedUse> uses;
    };

    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
};

} // namespace dayo::graphics
