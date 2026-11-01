// Reproduce the otherwise unused vertices.reserve() in refreshAnimatedMesh.
#include "core/frame_scratch.hpp"
#include "graphics/device.hpp"

#include <iostream>
#include <memory_resource>
#include <vector>

namespace {
class CountingResource final : public std::pmr::memory_resource {
  public:
    std::size_t allocations{}, bytes{};

  private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        bytes += size;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
} // namespace

int main() {
    CountingResource upstream;
    auto* previous = std::pmr::set_default_resource(&upstream);
    for (const std::size_t models : {1U, 3U, 5U}) {
        dayo::core::FrameScratch scratch;
        upstream.allocations = upstream.bytes = 0;
        {
            std::pmr::vector<dayo::graphics::PreviewVertex> vertices(scratch.resource());
            vertices.reserve(31'954U * models);
            std::cout << "{\"models\":" << models << ",\"vertex_bytes\":" << sizeof(dayo::graphics::PreviewVertex)
                      << ",\"reserved_bytes\":" << vertices.capacity() * sizeof(dayo::graphics::PreviewVertex)
                      << ",\"size\":" << vertices.size() << ",\"upstream_allocations\":" << upstream.allocations
                      << ",\"upstream_bytes\":" << upstream.bytes << "}\n";
        }
        scratch.reset();
    }
    std::pmr::set_default_resource(previous);
}
