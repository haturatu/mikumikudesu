#include <cstdlib>
#include <iostream>
#include <mmd/animation.hpp>
#include <new>

namespace {
bool collect = false;
std::size_t allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    if (collect)
        ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    mmd::PmxModel model;
    model.bones.resize(1);
    model.bones[0].name = "root";
    model.morphs.resize(1);
    model.morphs[0].type = 1;
    model.materials.resize(1);
    mmd::MmdAnimator animator(model);
    mmd::AnimatedModelFrame reused;
    animator.evaluateInto(reused, 0, 0, true, {}, mmd::VertexOutput::omitUnchangedGpu);
    for (const bool reuse : {false, true}) {
        allocations = 0;
        collect = true;
        for (unsigned i = 0; i < 1000; ++i) {
            if (reuse)
                animator.evaluateInto(reused, 0, 0, true, {}, mmd::VertexOutput::omitUnchangedGpu);
            else
                static_cast<void>(animator.evaluate(0, 0, true, {}, mmd::VertexOutput::omitUnchangedGpu));
        }
        collect = false;
        std::cout << "reuse=" << reuse << " allocations=" << allocations << '\n';
    }
}
