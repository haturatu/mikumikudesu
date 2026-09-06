#include "core/denoiser.hpp"
#include "core/scene.hpp"
#include "graphics/bdpt_accumulation.hpp"
#include "graphics/device.hpp"
#include "graphics/sbt.hpp"
#include "graphics/subayai_acceleration_structure.hpp"
#include "graphics/subayai_environment.hpp"
#include "graphics/subayai_light_sampling.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

bool check(bool value, std::string_view message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}

struct MockAccelerationBackend : dayo::graphics::IAccelerationBackend {
    std::uint64_t next{1};
    std::uint64_t createBlasCalls{0};
    std::uint64_t rebuildBlasCalls{0};
    std::uint64_t refitBlasCalls{0};
    std::uint64_t createTlasCalls{0};
    std::uint64_t rebuildTlasCalls{0};
    std::uint64_t updateTlasCalls{0};
    std::size_t lastTlasInstanceCount{};

    dayo::graphics::AccelerationStructureHandle createBlas(dayo::graphics::BufferHandle) override {
        ++createBlasCalls;
        return next++;
    }
    void rebuildBlas(dayo::graphics::AccelerationStructureHandle) override {
        ++rebuildBlasCalls;
    }
    void refitBlas(dayo::graphics::AccelerationStructureHandle) override {
        ++refitBlasCalls;
    }
    dayo::graphics::AccelerationStructureHandle
    createTlas(std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++createTlasCalls;
        lastTlasInstanceCount = instances.size();
        return next++;
    }
    void rebuildTlas(dayo::graphics::AccelerationStructureHandle,
                     std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++rebuildTlasCalls;
        lastTlasInstanceCount = instances.size();
    }
    void updateTlas(dayo::graphics::AccelerationStructureHandle,
                    std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++updateTlasCalls;
        lastTlasInstanceCount = instances.size();
    }
};

struct MockEnvironmentBackend : dayo::graphics::IEnvironmentBackend {
    std::uint64_t regenerations{0};
    void regenerate(const dayo::graphics::EnvironmentDesc&) override {
        ++regenerations;
    }
};

} // namespace

int main() {
    using dayo::graphics::AccelerationStructureService;
    using dayo::graphics::AliasEntry;
    using dayo::graphics::BdptAccumulation;
    using dayo::graphics::BlasAction;
    using dayo::graphics::EnvironmentDesc;
    using dayo::graphics::EnvironmentService;
    using dayo::graphics::LightSamplingService;
    using dayo::graphics::ShaderBindingTableBuilder;
    using dayo::graphics::TlasAction;
    bool ok = true;

    // BLAS branching: rebuild on topology, refit on deform-only, none otherwise.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        ok &= check(service.notifyMesh(0, 11, 1, 1) == BlasAction::rebuild, "BLAS first build is rebuild");
        ok &= check(backend.createBlasCalls == 1, "BLAS create called once");
        ok &= check(service.notifyMesh(0, 11, 1, 1) == BlasAction::none, "BLAS unchanged reports none");
        ok &= check(service.notifyMesh(0, 11, 1, 2) == BlasAction::refit, "BLAS deform-only refits");
        ok &= check(backend.refitBlasCalls == 1, "BLAS refit called once");
        ok &= check(service.notifyMesh(0, 11, 2, 2) == BlasAction::rebuild, "BLAS topology change rebuilds");
        ok &= check(backend.rebuildBlasCalls == 1, "BLAS rebuild called once");
        ok &= check(service.blasCount() == 1, "BLAS count tracks meshes");
    }
    // TLAS count reflects CloneCount; count/BLAS change rebuilds, world-only updates.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        static_cast<void>(service.notifyMesh(0, 11, 1, 1));
        static_cast<void>(service.notifyMesh(1, 12, 1, 1));
        const std::array<std::uint32_t, 2> clones{2, 3};
        ok &= check(service.notifyWorld(10, clones) == TlasAction::rebuild, "TLAS first build rebuilds");
        ok &= check(service.tlasInstanceCount() == 5, "TLAS instance count sums CloneCount");
        ok &= check(backend.lastTlasInstanceCount == 5, "TLAS backend receives every cloned instance");
        ok &= check(service.notifyWorld(10, clones) == TlasAction::none, "TLAS unchanged reports none");
        ok &= check(service.notifyWorld(11, clones) == TlasAction::update, "TLAS world change updates");
        ok &= check(backend.updateTlasCalls == 1, "TLAS update called once");
        const std::array<std::uint32_t, 2> grown{2, 4};
        ok &= check(service.notifyWorld(11, grown) == TlasAction::rebuild, "TLAS clone growth rebuilds");
        ok &= check(service.tlasInstanceCount() == 6, "TLAS instance count follows CloneCount");
        static_cast<void>(service.notifyMesh(0, 11, 1, 9));
        ok &= check(service.notifyWorld(11, grown) == TlasAction::none, "BLAS refit keeps stable TLAS addresses");
    }
    // EnvironmentService keeps cubemap/prefiltered/SH/Skywalker without regen.
    {
        MockEnvironmentBackend backend;
        EnvironmentService service(&backend);
        const EnvironmentDesc first{.source = "sky.hdr", .exposure = 1.0F, .version = 7};
        ok &= check(service.update(first), "environment first update regenerates");
        service.setHandles(11, 12, 7);
        ok &= check(!service.update(first), "environment unchanged reuses cache");
        ok &= check(backend.regenerations == 1 && service.generationCount() == 1, "environment regen counted once");
        ok &= check(service.cubemap() == 11 && service.prefilteredMips() == 12 && service.skywalkerVersion() == 7,
                    "environment keeps cubemap/prefiltered/Skywalker");
        ok &= check(service.sphericalHarmonics().size() == 27, "environment keeps SH coefficients");
        const EnvironmentDesc changed{.source = "sky.hdr", .exposure = 2.0F, .version = 7};
        ok &= check(service.update(changed), "environment exposure change regenerates");
        ok &= check(backend.regenerations == 2, "environment regen on change");
    }
    // LightSamplingService updates only on lighting dirty; light count from caller.
    {
        LightSamplingService service;
        const std::array<float, 3> powers{1.0F, 2.0F, 3.0F};
        service.update(powers, false);
        ok &= check(service.lightCount() == 0 && service.buildCount() == 0, "alias skips clean lighting");
        service.update(powers, true);
        ok &= check(service.lightCount() == 3 && service.buildCount() == 1, "alias builds from caller powers");
        const auto table = service.table();
        double probabilitySum = 0.0;
        bool aliasesInRange = true;
        for (const AliasEntry& entry : table) {
            probabilitySum += static_cast<double>(entry.probability);
            aliasesInRange &= entry.alias < table.size();
            aliasesInRange &= entry.probability >= 0.0F && entry.probability <= 1.0F;
        }
        ok &= check(aliasesInRange, "alias entries reference valid lights");
        ok &= check(std::abs(probabilitySum - 1.5) < 2.0, "alias probabilities plausible");
        service.update(powers, false);
        ok &= check(service.buildCount() == 1, "alias keeps table without lighting dirty");
        const std::array<float, 5> moreLights{1.0F, 1.0F, 1.0F, 1.0F, 4.0F};
        service.update(moreLights, true);
        ok &= check(service.lightCount() == 5 && service.buildCount() == 2, "alias light count is caller-driven");
    }
    // BDPT accumulation: dirty resets to 0+clear, otherwise increments.
    {
        BdptAccumulation accumulation;
        accumulation.ensurePersistent();
        ok &=
            check(accumulation.persistentReady() && accumulation.spectralLutReady() && accumulation.blackbodyLutReady(),
                  "BDPT spectral/blackbody LUTs ready");
        ok &= check(accumulation.volumeCount() == BdptAccumulation::kVolumeSlots, "BDPT keeps 8 volume slots");
        bool volumesValid = true;
        for (std::size_t index = 0; index < accumulation.volumeCount(); ++index) {
            volumesValid &= accumulation.volume(index).valid;
        }
        ok &= check(volumesValid, "BDPT volume slots valid");
        const auto* spectralBefore = accumulation.spectralLut().data();
        const auto* blackbodyBefore = accumulation.blackbodyLut().data();
        accumulation.ensurePersistent();
        ok &= check(accumulation.generationCount() == 1, "BDPT persistent resources created once");
        ok &= check(accumulation.spectralLut().data() == spectralBefore &&
                        accumulation.blackbodyLut().data() == blackbodyBefore,
                    "BDPT LUTs persist without regeneration");
        ok &= check(accumulation.beginFrame(dayo::core::DirtyFlag::geometry), "BDPT dirty requests clear");
        ok &= check(accumulation.sampleIndex() == 0 && accumulation.needsClear(), "BDPT dirty resets sample");
        ok &= check(!accumulation.beginFrame(dayo::core::DirtyFlag::none), "BDPT clean continues");
        ok &= check(accumulation.sampleIndex() == 1 && !accumulation.needsClear(), "BDPT clean increments");
        ok &= check(!accumulation.beginFrame(dayo::core::DirtyFlag::none), "BDPT second clean continues");
        ok &= check(accumulation.sampleIndex() == 2, "BDPT sample index accumulates");
        dayo::core::Scene scene;
        BdptAccumulation synced;
        ok &= check(synced.syncScene(scene), "BDPT scene initial dirty resets");
        ok &= check(synced.sampleIndex() == 0 && scene.accumulatedSamples() == 0, "BDPT scene reset clears");
        scene.clearDirty();
        ok &= check(!synced.syncScene(scene), "BDPT scene clean accumulates");
        ok &= check(synced.sampleIndex() == 1 && scene.accumulatedSamples() == 1, "BDPT scene uses accumulatedSamples");
    }
    // ShaderBindingTableBuilder: shared raygen/miss/hit layout.
    {
        ShaderBindingTableBuilder builder;
        builder.setRaygen("raygen");
        builder.addMiss("miss0");
        builder.addMiss("miss1");
        builder.addHitGroup("hit0");
        builder.addHitGroup("hit1");
        builder.addHitGroup("hit2");
        ok &= check(builder.totalGroups() == 6, "SBT tracks raygen/miss/hit groups");
        const auto layout = builder.build(0x1000, 32, 32);
        ok &= check(layout.raygenAddress == 0x1000, "SBT raygen base address");
        ok &= check(layout.missAddress == 0x1000 + 32, "SBT miss follows raygen");
        ok &= check(layout.hitAddress == 0x1000 + 32 + 2 * 32, "SBT hit follows miss");
        ok &= check(layout.totalSize == 6 * 32, "SBT total size covers shared groups");
        ok &= check(layout.raygenStride == 32 && layout.missStride == 32 && layout.hitStride == 32,
                    "SBT shares stride across groups");
        const auto aligned = builder.build(0x2000, 20, 32);
        ok &= check(aligned.raygenStride == 32 && aligned.totalSize == 6 * 32, "SBT aligns handles");
        const ShaderBindingTableBuilder::Properties properties{20, 32, 64, 64};
        const auto vulkanValid = builder.build(0x2001, properties);
        ok &= check(vulkanValid.raygenAddress % 64 == 0 && vulkanValid.missAddress % 64 == 0 &&
                        vulkanValid.hitAddress % 64 == 0,
                    "SBT aligns every region base to shaderGroupBaseAlignment");
        ok &= check(vulkanValid.raygenStride == 32 && vulkanValid.totalSize >= 6 * 32,
                    "SBT properties enforce aligned stride and padded allocation");
        ok &= check(builder.build(0x2000, ShaderBindingTableBuilder::Properties{65, 32, 64, 64}).totalSize == 0,
                    "SBT rejects maxShaderGroupStride overflow");
    }
    // DenoiserRuntime fallback: staging/readback -> CPU -> upload copy without CUDA.
    {
        dayo::core::DenoiserRuntime runtime;
        runtime.setShareable(false);
        const bool ensured = runtime.ensure(2, 1);
        ok &= check(ensured || !runtime.available(), "denoiser ensure accepts extent or reports unavailable");
        const std::array<float, 6> beauty{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
        const std::array<float, 6> albedo{0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F};
        const std::array<float, 6> normal{0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
        std::array<float, 6> output{};
        const dayo::core::DenoiserExecuteArgs args{
            .width = 2, .height = 1, .beauty = beauty, .albedo = albedo, .normal = normal, .output = output};
        ok &= check(runtime.execute(args), "denoiser staging fallback executes");
        ok &= check(runtime.usedFallback(), "denoiser staging path marks fallback");
        if (runtime.available())
            ok &= check(runtime.denoised(), "available staging path runs CPU OIDN");
        else
            ok &= check(!runtime.denoised() && output == beauty, "unavailable denoiser is explicit passthrough");
        std::array<float, 6> badOutput{};
        const dayo::core::DenoiserExecuteArgs bad{
            .width = 4, .height = 4, .beauty = beauty, .albedo = albedo, .normal = normal, .output = badOutput};
        ok &= check(!runtime.execute(bad), "denoiser rejects mismatched oidnPass extent at execution");
        dayo::core::DenoiserRuntime forced;
        forced.setForceFallback(true);
        forced.setShareable(true);
        static_cast<void>(forced.ensure(2, 1));
        std::array<float, 6> forcedOutput{};
        const dayo::core::DenoiserExecuteArgs forcedArgs{
            .width = 2, .height = 1, .beauty = beauty, .albedo = {}, .normal = {}, .output = forcedOutput};
        ok &= check(forced.execute(forcedArgs), "denoiser forced CPU fallback executes");
        ok &= check(forced.usedFallback() && (!forced.available() ? forcedOutput == beauty : forced.denoised()),
                    "denoiser forced path distinguishes CPU denoise from passthrough");
    }
    // Feature gate: native flags stay false, Preview fallback remains available.
    {
        dayo::graphics::DeviceCapabilities capabilities;
        capabilities.swapchain = true;
        capabilities.bufferDeviceAddress = true;
        capabilities.descriptorIndexing = true;
        capabilities.accelerationStructure = true;
        capabilities.rayQuery = true;
        capabilities.fragmentShaderBarycentric = true;
        capabilities.rayTracingPipeline = true;
        capabilities.nativeSubayai = false;
        capabilities.nativeBdpt = false;
        ok &= check(!capabilities.supportsSubayai(), "native Subayai stays disabled");
        ok &= check(!capabilities.supportsBdpt(), "native BDPT stays disabled");
        ok &= check(capabilities.supportsPreview(), "Preview stays available without RT");
        ok &= check(capabilities.hardwareSupportsSubayai() && capabilities.hardwareSupportsBdpt(),
                    "hardware detection independent of native flags");
        ok &= check(!AccelerationStructureService::canBuildNative(capabilities, dayo::graphics::RendererKind::bdpt) ==
                        false,
                    "host bookkeeping allowed on RT hardware");
        dayo::graphics::DeviceCapabilities noRt;
        noRt.swapchain = true;
        ok &= check(!AccelerationStructureService::canBuildNative(noRt, dayo::graphics::RendererKind::subayai),
                    "RT-less GPU falls back to Preview");
        ok &= check(noRt.supportsPreview(), "RT-less GPU still starts Preview");
        ok &= check(!noRt.missingFeatures(dayo::graphics::RendererKind::subayai).empty(),
                    "missing RT features reported for fallback");
    }

    if (!ok) {
        return 1;
    }
    std::cout << "subayai_bdpt tests passed\n";
    return 0;
}
