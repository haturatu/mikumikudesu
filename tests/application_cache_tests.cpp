#include "app/application.hpp"
#include "mock_native_device.hpp"

#include <mmd/pmx.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace dayo::app {
struct ApplicationCacheTestAccess {
    static void run(Application& app, test::MockNativeDevice& device, const std::filesystem::path& modelPath) {
        const auto require = [](bool value, const char* message) {
            if (!value)
                throw std::runtime_error(message);
        };
        app.device_ = &device;
        device.selectRenderer(graphics::RendererKind::preview);
        core::EffectGraph effect;
        effect.category = "render";
        app.scene_.setEffect(std::move(effect));
        const auto first = app.scene_.addModel(modelPath);
        app.animationFrame_ = 1.0F;
        app.scene_.setFrame(app.animationFrame_);
        app.refreshAnimatedMesh(true, 1.0F / 60.0F);
        require(app.nativeGeometry_.empty() && app.nativeSceneModelData_.empty(),
                "Preview must not construct native geometry or model tables");
        require(app.evaluatedFrames_[0].frame.vertices.empty(), "ordinary GPU skinning must omit CPU vertex copies");
        const auto before = app.scene_.model(first)->physics->stepStats().substeps;
        const auto body = app.scene_.model(first)->physics->bodyTransform(0);
        std::string error;
        require(app.ensureNativeSceneRuntime(false, &error), error.c_str());
        require(app.nativeSceneModelData_.size() == 1 && app.nativeSceneResources_.counts().vertexBuffers == 1,
                "native descriptors must be sized after building the current model data");
        require(app.scene_.model(first)->physics->stepStats().substeps == before &&
                    app.scene_.model(first)->physics->bodyTransform(0).position == body.position,
                "renderer initialization must reuse the pose without evaluating physics again");
        require(app.nativeSceneModelData_[0].vertices.size() == app.scene_.model(first)->model->vertices.size(),
                "native switch must reconstruct omitted bind attributes without reevaluation");
        const auto* raw = app.nativeSceneModelData_[0].rawVertices.data();
        const auto topology = app.nativeSceneModelData_[0].topologyGeneration;
        const auto material = app.nativeSceneModelData_[0].materialGeneration;
        require(app.refreshNativeSceneData() && raw == app.nativeSceneModelData_[0].rawVertices.data() &&
                    topology == app.nativeSceneModelData_[0].topologyGeneration &&
                    material == app.nativeSceneModelData_[0].materialGeneration,
                "unchanged native inputs must retain static buffers and material generations");
        app.requestRenderer(graphics::RendererKind::preview);
        const auto generation = app.nativeDeformVersion_;
        app.animationFrame_ = 2.0F;
        app.scene_.setFrame(app.animationFrame_);
        app.refreshAnimatedMesh(false, 1.0F / 30.0F);
        require(app.nativeDeformVersion_ == generation, "Preview pose updates must not rebuild native caches");
        const auto second = app.scene_.addModel(modelPath);
        app.requestRenderer(graphics::RendererKind::subayai);
        require(app.nativePreparePending_,
                "a topology change must defer native initialization until a current pose exists");
        app.refreshAnimatedMesh(false, 1.0F / 60.0F);
        require(app.nativeSceneModelData_.size() == 2 && app.nativeSceneResources_.counts().vertexBuffers == 2,
                "adding a model during Preview must resize native descriptors before renderer preparation");
        app.requestRenderer(graphics::RendererKind::preview);
        require(app.scene_.removeModel(first), "fixture model removal failed");
        app.requestRenderer(graphics::RendererKind::subayai);
        require(app.nativePreparePending_, "removing a model must invalidate the evaluated topology");
        app.refreshAnimatedMesh(false, 1.0F / 60.0F);
        require(app.nativeGeometry_.size() == 1 && app.nativeGeometry_[0].modelId == second &&
                    app.nativeSceneResources_.counts().vertexBuffers == 1,
                "model removal must not retain the previous native model identity or descriptor count");
        app.scene_.clearModels();
        app.refreshAnimatedMesh(false);
        require(app.nativeSceneModelData_.empty() && app.evaluatedModels_.models.empty(),
                "clearing models must clear native data and controller snapshots");
        auto uvModel = mmd::pmx::load(modelPath);
        uvModel.morphs.resize(1);
        uvModel.morphs[0].name = "uv";
        uvModel.morphs[0].type = 3;
        uvModel.morphs[0].offsets = {{.index = 0, .vector4 = {0.25F, 0, 0, 0}}};
        const auto uvPath = modelPath.parent_path() / "uv.pmx";
        static_cast<void>(mmd::pmx::save(uvPath, uvModel));
        const auto uvId = app.scene_.addModel(uvPath);
        auto* instance = app.scene_.model(uvId);
        instance->motion = std::make_unique<mmd::VmdMotion>();
        instance->motion->morphs = {{"uv", 0, 0.0F}, {"uv", 30, 1.0F}};
        instance->animator->setMotion(instance->motion.get());
        app.requestRenderer(graphics::RendererKind::preview);
        app.animationFrame_ = 0;
        app.scene_.setFrame(0);
        app.refreshAnimatedMesh(true);
        require(device.previewVertices[0].uv[0] == 0.0F, "UV fixture starts with bind UVs");
        app.animationFrame_ = 30;
        app.scene_.setFrame(30);
        app.refreshAnimatedMesh(false);
        require(app.evaluatedFrames_[0].frame.vertices.size() == 3 && device.previewVertices[0].uv[0] == 0.25F,
                "GPU skinning must retain and upload changing CPU UV morphs on ordinary frames");
        // Keep the mock alive through all application-owned GPU resource destructors.
    }
};
} // namespace dayo::app

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("dayo-cache-" + dayo::core::newModelInstanceId());
    std::filesystem::create_directory(directory);
    try {
        mmd::PmxModel model;
        model.metadata.version = 2.0F;
        model.metadata.modelName = "native cache fixture";
        model.vertices.resize(3);
        model.vertices[1].position = {1.0F, 0.0F, 0.0F};
        model.vertices[2].position = {0.0F, 1.0F, 0.0F};
        for (auto& vertex : model.vertices) {
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.bones[0] = 0;
        }
        model.indices = {0, 1, 2};
        model.materials.resize(1);
        model.materials[0].diffuse = {1.0F, 1.0F, 1.0F, 1.0F};
        model.materials[0].indexCount = 3;
        model.bones.resize(1);
        model.bones[0].name = "root";
        model.rigidBodies.resize(1);
        model.rigidBodies[0].bone = 0;
        model.rigidBodies[0].mode = 1;
        model.rigidBodies[0].mass = 1.0F;
        model.rigidBodies[0].size = {0.1F, 0.1F, 0.1F};
        model.rigidBodies[0].position = {0.0F, 1.0F, 0.0F};
        const auto modelPath = directory / "model.pmx";
        static_cast<void>(mmd::pmx::save(modelPath, model));
        dayo::test::MockNativeDevice device;
        dayo::app::Application application({});
        dayo::app::ApplicationCacheTestAccess::run(application, device, modelPath);
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
    return 0;
}
