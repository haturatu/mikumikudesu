#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace dayo::graphics::handles {

inline constexpr std::uint32_t kInvalidHandleIndex = std::numeric_limits<std::uint32_t>::max();

struct BufferHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const BufferHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const BufferHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

struct TextureHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const TextureHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const TextureHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

struct SamplerHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const SamplerHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const SamplerHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

struct DescriptorSetHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const DescriptorSetHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const DescriptorSetHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

struct PipelineHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const PipelineHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const PipelineHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

struct ShaderBindingTableHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const ShaderBindingTableHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const ShaderBindingTableHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

struct DescriptorSetLayoutHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const DescriptorSetLayoutHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const DescriptorSetLayoutHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

struct PipelineLayoutHandle {
    std::uint32_t index{kInvalidHandleIndex};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kInvalidHandleIndex;
    }
    [[nodiscard]] constexpr bool operator==(const PipelineLayoutHandle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] constexpr bool operator!=(const PipelineLayoutHandle& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

// Generation-tagged pool with stale detection. Destroying a handle bumps the
// generation so any retained copy fails isAlive()/destroy() instead of
// aliasing a recycled slot.
template <typename Handle> class GenerationRegistry {
  public:
    [[nodiscard]] Handle create() {
        if (!free_.empty()) {
            const std::uint32_t index = free_.back();
            free_.pop_back();
            Slot& slot = slots_[index];
            slot.alive = true;
            return Handle{index, slot.generation};
        }
        const std::uint32_t index = static_cast<std::uint32_t>(slots_.size());
        // Generations start at 1 so a default-constructed handle (generation 0
        // with a recycled index) never compares alive.
        slots_.push_back(Slot{1, true});
        return Handle{index, 1};
    }

    // Returns false when the handle is invalid or stale (generation mismatch).
    bool destroy(Handle handle) {
        if (!handle.valid() || handle.index >= slots_.size())
            return false;
        Slot& slot = slots_[handle.index];
        if (!slot.alive || slot.generation != handle.generation)
            return false;
        slot.alive = false;
        ++slot.generation;
        if (slot.generation == 0)
            slot.generation = 1;
        free_.push_back(handle.index);
        return true;
    }

    [[nodiscard]] bool isAlive(Handle handle) const noexcept {
        if (!handle.valid() || handle.index >= slots_.size())
            return false;
        const Slot& slot = slots_[handle.index];
        return slot.alive && slot.generation == handle.generation;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return slots_.size();
    }

    [[nodiscard]] std::size_t aliveCount() const noexcept {
        std::size_t count = 0;
        for (const auto& slot : slots_)
            count += slot.alive ? 1U : 0U;
        return count;
    }

    void clear() noexcept {
        slots_.clear();
        free_.clear();
    }

  private:
    struct Slot {
        std::uint32_t generation{};
        bool alive{};
    };
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
};

using BufferPool = GenerationRegistry<BufferHandle>;
using TexturePool = GenerationRegistry<TextureHandle>;
using SamplerPool = GenerationRegistry<SamplerHandle>;
using DescriptorSetPool = GenerationRegistry<DescriptorSetHandle>;
using PipelinePool = GenerationRegistry<PipelineHandle>;
using ShaderBindingTablePool = GenerationRegistry<ShaderBindingTableHandle>;
using DescriptorSetLayoutPool = GenerationRegistry<DescriptorSetLayoutHandle>;
using PipelineLayoutPool = GenerationRegistry<PipelineLayoutHandle>;

// RAII guard that returns the handle to the pool on destruction. Move-only so
// ownership transfer is explicit and stale copies are still detected via the
// pool generation check.
template <typename Handle> class ScopedHandle {
  public:
    ScopedHandle() noexcept = default;
    ScopedHandle(GenerationRegistry<Handle>* pool, Handle handle) noexcept : pool_(pool), handle_(handle) {}
    ~ScopedHandle() {
        reset();
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : pool_(other.pool_), handle_(other.handle_) {
        other.pool_ = nullptr;
        other.handle_ = Handle{};
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            pool_ = other.pool_;
            handle_ = other.handle_;
            other.pool_ = nullptr;
            other.handle_ = Handle{};
        }
        return *this;
    }

    void reset() noexcept {
        if (pool_ != nullptr && handle_.valid()) {
            // Destroy may fail if the handle was already retired externally;
            // that staleness is intentional and not an error here.
            static_cast<void>(pool_->destroy(handle_));
        }
        pool_ = nullptr;
        handle_ = Handle{};
    }

    [[nodiscard]] Handle get() const noexcept {
        return handle_;
    }
    [[nodiscard]] bool alive() const noexcept {
        return pool_ != nullptr && pool_->isAlive(handle_);
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return alive();
    }

  private:
    GenerationRegistry<Handle>* pool_{nullptr};
    Handle handle_{};
};

} // namespace dayo::graphics::handles

namespace std {

template <> struct hash<dayo::graphics::handles::BufferHandle> {
    std::size_t operator()(dayo::graphics::handles::BufferHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};
template <> struct hash<dayo::graphics::handles::TextureHandle> {
    std::size_t operator()(dayo::graphics::handles::TextureHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};
template <> struct hash<dayo::graphics::handles::SamplerHandle> {
    std::size_t operator()(dayo::graphics::handles::SamplerHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};
template <> struct hash<dayo::graphics::handles::DescriptorSetHandle> {
    std::size_t operator()(dayo::graphics::handles::DescriptorSetHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};
template <> struct hash<dayo::graphics::handles::PipelineHandle> {
    std::size_t operator()(dayo::graphics::handles::PipelineHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};
template <> struct hash<dayo::graphics::handles::ShaderBindingTableHandle> {
    std::size_t operator()(dayo::graphics::handles::ShaderBindingTableHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};
template <> struct hash<dayo::graphics::handles::DescriptorSetLayoutHandle> {
    std::size_t operator()(dayo::graphics::handles::DescriptorSetLayoutHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};
template <> struct hash<dayo::graphics::handles::PipelineLayoutHandle> {
    std::size_t operator()(dayo::graphics::handles::PipelineLayoutHandle handle) const noexcept {
        return (static_cast<std::size_t>(handle.index) << 32U) | handle.generation;
    }
};

} // namespace std
