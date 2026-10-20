#include <chrono>
#include <iostream>
#include <mmd/physics.hpp>
int main() {
    mmd::PmxModel model;
    model.rigidBodies.resize(256);
    for (std::size_t i = 0; i < model.rigidBodies.size(); ++i) {
        auto& body = model.rigidBodies[i];
        body.bone = -1;
        body.mode = 1;
        body.mass = 1;
        body.size = {0.3F, 0.3F, 0.3F};
        body.position = {static_cast<float>(i % 16), 0.25F, static_cast<float>(i / 16)};
        body.collisionMask = 0xffff;
    }
    for (unsigned phase = 0; phase < 6; ++phase) {
        mmd::PhysicsSettings settings;
        settings.collectContactDiagnostics = phase % 2 == 0;
        mmd::MmdPhysics physics(model, settings);
        physics.setFloorCollision(true);
        for (unsigned i = 0; i < 60; ++i)
            physics.step(1.0F / 60.0F);
        std::size_t contacts = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 600; ++i) {
            physics.step(1.0F / 60.0F);
            contacts += physics.stepStats().contacts.size();
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        double heightSum = 0;
        for (std::size_t i = 0; i < model.rigidBodies.size(); ++i)
            heightSum += physics.bodyTransform(i).position[1];
        std::cout << "collect=" << settings.collectContactDiagnostics << " mean=" << ms / 600
                  << " contacts=" << contacts << " heightSum=" << heightSum << '\n';
    }
}
