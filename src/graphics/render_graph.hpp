#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
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
        auto addDependency = [](std::vector<RenderGraphPass>& dependencies, RenderGraphPass pass) {
            if (pass != invalidPass && std::find(dependencies.begin(), dependencies.end(), pass) == dependencies.end())
                dependencies.push_back(pass);
        };

        for (std::uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            const auto& pass = passes_[passIndex];
            RenderGraphPassPlan plan{pass.name, {}, {}};
            for (const auto& use : pass.uses) {
                if (use.resource >= resources_.size())
                    throw std::invalid_argument("render graph use references an unknown resource");
                auto& resource = tracking[use.resource];
                if (use.write) {
                    addDependency(plan.dependencies, resource.lastWriter);
                    for (const auto reader : resource.readers)
                        addDependency(plan.dependencies, reader);
                    resource.readers.clear();
                    resource.lastWriter = passIndex;
                } else {
                    addDependency(plan.dependencies, resource.lastWriter);
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

} // namespace dayo::graphics
