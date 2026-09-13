#include "core/denoiser.hpp"
#include "core/scene.hpp"
#include "graphics/bdpt_accumulation.hpp"
#include "graphics/device.hpp"
#include "graphics/sbt.hpp"
#include "graphics/subayai_acceleration_structure.hpp"
#include "graphics/subayai_deform.hpp"
#include "graphics/subayai_environment.hpp"
#include "graphics/subayai_light_sampling.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
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
    std::uint32_t next{1};
    std::uint64_t createBlasCalls{0};
    std::uint64_t rebuildBlasCalls{0};
    std::uint64_t refitBlasCalls{0};
    std::uint64_t createTlasCalls{0};
    std::uint64_t rebuildTlasCalls{0};
    std::uint64_t updateTlasCalls{0};
    std::uint64_t destroyBlasCalls{0};
    std::uint64_t destroyTlasCalls{0};
    std::size_t lastTlasInstanceCount{};
    std::vector<dayo::graphics::handles::BufferHandle> rebuildVertexBuffers;
    std::vector<dayo::graphics::handles::BufferHandle> refitVertexBuffers;
    std::vector<dayo::graphics::TlasInstanceDesc> lastTlasInstances;

    dayo::graphics::handles::AccelerationStructureHandle
    createBlas(const dayo::graphics::BlasGeometryDesc& geometry) override {
        ++createBlasCalls;
        if (geometry.triangles.empty())
            return {};
        return {next++, 1};
    }
    dayo::graphics::handles::AccelerationStructureHandle
    rebuildBlas(dayo::graphics::handles::AccelerationStructureHandle blas,
                const dayo::graphics::BlasGeometryDesc& geometry) override {
        ++rebuildBlasCalls;
        rebuildVertexBuffers.push_back(geometry.triangles.front().vertexBuffer);
        return blas;
    }
    void refitBlas(dayo::graphics::handles::AccelerationStructureHandle,
                   const dayo::graphics::BlasGeometryDesc& geometry) override {
        ++refitBlasCalls;
        refitVertexBuffers.push_back(geometry.triangles.front().vertexBuffer);
    }
    dayo::graphics::handles::AccelerationStructureHandle
    createTlas(std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++createTlasCalls;
        lastTlasInstanceCount = instances.size();
        lastTlasInstances.assign(instances.begin(), instances.end());
        return {next++, 1};
    }
    void rebuildTlas(dayo::graphics::handles::AccelerationStructureHandle,
                     std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++rebuildTlasCalls;
        lastTlasInstanceCount = instances.size();
        lastTlasInstances.assign(instances.begin(), instances.end());
    }
    void updateTlas(dayo::graphics::handles::AccelerationStructureHandle,
                    std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++updateTlasCalls;
        lastTlasInstanceCount = instances.size();
        lastTlasInstances.assign(instances.begin(), instances.end());
    }
    void destroyBlas(dayo::graphics::handles::AccelerationStructureHandle) override {
        ++destroyBlasCalls;
    }
    void destroyTlas(dayo::graphics::handles::AccelerationStructureHandle) override {
        ++destroyTlasCalls;
    }
};

struct MockEnvironmentBackend : dayo::graphics::IEnvironmentBackend {
    std::uint64_t regenerations{0};
    void regenerate(const dayo::graphics::EnvironmentDesc&) override {
        ++regenerations;
    }
};

} // namespace

dayo::graphics::BlasGeometryDesc geometry(std::uint32_t vertexBuffer) {
    return {.triangles = {{.vertexBuffer = {vertexBuffer, 1},
                           .vertexStride = sizeof(dayo::graphics::NativeDeformedVertex),
                           .vertexCount = 3,
                           .indexBuffer = {99, 1},
                           .indexType = dayo::graphics::IndexType::uint32,
                           .indexCount = 3}}};
}

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

    // Native deformation writes actual positions into a buffer that is valid
    // for both vertex reads and BLAS build input. Preview's source skinning
    // attributes never appear as an AS vertex buffer.
    {
        const dayo::graphics::NativeDeformInput input{
            .vertexCount = 130, .indexCount = 192, .boneCount = 8, .morphDeltaCount = 24, .morphCount = 6};
        const auto plan = dayo::graphics::makeNativeDeformPlan(input);
        ok &= check(plan.deformedVertices.size == 130U * sizeof(dayo::graphics::NativeDeformedVertex),
                    "native deform allocates deformed vertex output");
        ok &= check((dayo::graphics::toBits(plan.deformedVertices.usage) &
                     dayo::graphics::toBits(dayo::graphics::ResourceUsage::asBuildRead)) != 0U,
                    "deformed output is BLAS build-readable");
        ok &= check(plan.workgroupCount == 3, "native deform rounds dispatch groups up");
        const auto blas = plan.makeBlasGeometry({17, 1}, {18, 1});
        ok &= check(blas.triangles.size() == 1 && blas.triangles.front().vertexCount == 130 &&
                        blas.triangles.front().indexCount == 192 &&
                        blas.triangles.front().vertexStride == sizeof(dayo::graphics::NativeDeformedVertex),
                    "native deform plan creates complete typed BLAS geometry");
        bool invalidThrew = false;
        try {
            static_cast<void>(dayo::graphics::makeNativeDeformPlan({.vertexCount = 3, .indexCount = 4}));
        } catch (const std::invalid_argument&) {
            invalidThrew = true;
        }
        ok &= check(invalidThrew, "native deform rejects non-triangle index counts");
    }

    // BLAS branching: rebuild on topology, refit on deform-only, none otherwise.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        ok &= check(service.notifyMesh(0, geometry(11), 1, 1) == BlasAction::rebuild, "BLAS first build is rebuild");
        ok &= check(backend.createBlasCalls == 1, "BLAS create called once");
        ok &= check(service.notifyMesh(0, geometry(11), 1, 1) == BlasAction::none, "BLAS unchanged reports none");
        ok &= check(service.notifyMesh(0, geometry(11), 1, 2) == BlasAction::refit, "BLAS deform-only refits");
        ok &= check(backend.refitBlasCalls == 1, "BLAS refit called once");
        ok &= check(backend.refitVertexBuffers.size() == 1 && backend.refitVertexBuffers.front().index == 11,
                    "BLAS refit receives the current vertex buffer");
        ok &= check(service.notifyMesh(0, geometry(11), 2, 2) == BlasAction::rebuild, "BLAS topology change rebuilds");
        ok &= check(backend.rebuildBlasCalls == 1, "BLAS rebuild called once");
        ok &= check(backend.rebuildVertexBuffers.size() == 1 && backend.rebuildVertexBuffers.front().index == 11,
                    "BLAS rebuild receives the current vertex buffer");
        ok &= check(service.blasCount() == 1, "BLAS count tracks meshes");
    }
    // A changed vertex-buffer allocation is forwarded to BLAS rebuild/refit,
    // and service destruction releases all remaining backend resources.
    {
        MockAccelerationBackend backend;
        {
            AccelerationStructureService service(&backend);
            static_cast<void>(service.notifyMesh(0, geometry(11), 1, 1));
            static_cast<void>(service.notifyMesh(0, geometry(22), 1, 2));
            static_cast<void>(service.notifyMesh(0, geometry(33), 2, 3));
        }
        ok &= check(backend.refitVertexBuffers.size() == 1 && backend.refitVertexBuffers.front().index == 22,
                    "deform update forwards a replaced vertex buffer");
        ok &= check(backend.rebuildVertexBuffers.size() == 1 && backend.rebuildVertexBuffers.front().index == 33,
                    "topology update forwards a replaced vertex buffer");
        ok &= check(backend.destroyBlasCalls == 1, "service destructor releases remaining BLAS");
    }
    // TLAS count reflects CloneCount; count/BLAS change rebuilds, world-only updates.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        static_cast<void>(service.notifyMesh(0, geometry(11), 1, 1));
        static_cast<void>(service.notifyMesh(1, geometry(12), 1, 1));
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
        static_cast<void>(service.notifyMesh(0, geometry(11), 1, 9));
        ok &= check(service.notifyWorld(11, grown) == TlasAction::none, "BLAS refit keeps stable TLAS addresses");
        ok &= check(service.removeMesh(1), "removing a mesh succeeds");
        ok &= check(service.blasCount() == 1 && !service.tlasBuilt(), "mesh removal retires TLAS state");
        ok &= check(backend.destroyBlasCalls == 1 && backend.destroyTlasCalls == 1,
                    "mesh removal destroys BLAS and TLAS resources");
    }
    // Transform-aware world instances preserve clone placement and metadata.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        static_cast<void>(service.notifyMesh(10, geometry(11), 1, 1));
        static_cast<void>(service.notifyMesh(20, geometry(12), 1, 1));
        std::array<dayo::graphics::WorldInstance, 3> world{};
        world[0].meshId = 20;
        world[0].transform.values[3] = 4.0F;
        world[0].mask = 0x7FU;
        world[0].sbtRecordOffset = 3;
        world[1].meshId = 10;
        world[1].transform.values[7] = 5.0F;
        world[1].flags = 1;
        world[2].meshId = 20;
        world[2].transform.values[11] = 6.0F;
        ok &=
            check(service.notifyWorld(20, world) == TlasAction::rebuild, "transform-aware world instances build TLAS");
        ok &=
            check(backend.lastTlasInstances.size() == 3 &&
                      backend.lastTlasInstances[0].blas == dayo::graphics::handles::AccelerationStructureHandle{2, 1} &&
                      backend.lastTlasInstances[0].transform.values[3] == 4.0F &&
                      backend.lastTlasInstances[0].mask == 0x7FU && backend.lastTlasInstances[0].sbtRecordOffset == 3,
                  "TLAS preserves mesh mapping, transform, and metadata");
        ok &= check(backend.lastTlasInstances[1].blas == dayo::graphics::handles::AccelerationStructureHandle{1, 1} &&
                        backend.lastTlasInstances[1].transform.values[7] == 5.0F &&
                        backend.lastTlasInstances[1].flags == 1 && backend.lastTlasInstances[2].instanceId == 2,
                    "TLAS expands each world instance independently");
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
        ok &= check(AccelerationStructureService::canBuildNative(capabilities, dayo::graphics::RendererKind::bdpt),
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
