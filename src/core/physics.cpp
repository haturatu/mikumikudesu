#include "core/physics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

#if DAYO_HAS_BULLET
#include <btBulletDynamicsCommon.h>
#endif

namespace dayo::core {

struct MmdPhysics::Impl {
#if DAYO_HAS_BULLET
    std::unique_ptr<btDefaultCollisionConfiguration> collisionConfiguration;
    std::unique_ptr<btCollisionDispatcher> dispatcher;
    std::unique_ptr<btDbvtBroadphase> broadphase;
    std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
    std::unique_ptr<btDiscreteDynamicsWorld> world;
    std::vector<std::unique_ptr<btCollisionShape>> shapes;
    std::vector<std::unique_ptr<btDefaultMotionState>> motionStates;
    std::vector<std::unique_ptr<btRigidBody>> bodies;
    std::vector<std::unique_ptr<btTypedConstraint>> constraints;
    std::vector<btTransform> initialTransforms;
    std::vector<btTransform> kinematicStarts;
    std::vector<btTransform> kinematicTargets;
    std::vector<std::uint8_t> kinematicDirty;
    std::vector<std::uint8_t> modes;
    std::unique_ptr<btStaticPlaneShape> floorShape;
    std::unique_ptr<btDefaultMotionState> floorMotionState;
    std::unique_ptr<btRigidBody> floorBody;
    Float3 gravity{0.0F, -98.0F, 0.0F};
    float gravityNoiseAmplitude{};
    float gravityNoiseFrequency{};
    float elapsed{};
    bool floorCollision{};

    ~Impl() {
        if (world == nullptr)
            return;
        for (auto& constraint : constraints)
            world->removeConstraint(constraint.get());
        if (floorBody != nullptr)
            world->removeRigidBody(floorBody.get());
        for (auto& body : bodies)
            world->removeRigidBody(body.get());
    }
#endif
};

#if DAYO_HAS_BULLET
namespace {

btVector3 vector(const Float3& value) {
    return {value[0], value[1], value[2]};
}
Float3 vector(const btVector3& value) {
    return {value.x(), value.y(), value.z()};
}

bool finite(const Float3& value) {
    return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
}

btQuaternion rotation(const Float3& euler) {
    btQuaternion value;
    value.setEulerZYX(euler[2], euler[1], euler[0]);
    return value;
}

btTransform transform(const Float3& position, const Float3& euler) {
    return btTransform(rotation(euler), vector(position));
}

btTransform transform(const PhysicsTransform& value) {
    return btTransform(btQuaternion(value.rotation[0], value.rotation[1], value.rotation[2], value.rotation[3]),
                       vector(value.position));
}

btTransform interpolate(const btTransform& from, const btTransform& to, btScalar amount) {
    const auto rotation = from.getRotation().slerp(to.getRotation(), amount);
    const auto position = from.getOrigin() * (1.0F - amount) + to.getOrigin() * amount;
    return btTransform(rotation, position);
}

bool finite(const PhysicsTransform& value) {
    return std::ranges::all_of(value.position, [](float component) { return std::isfinite(component); }) &&
           std::ranges::all_of(value.rotation, [](float component) { return std::isfinite(component); });
}

PhysicsTransform transform(const btTransform& value) {
    const auto& q = value.getRotation();
    return {vector(value.getOrigin()), {q.x(), q.y(), q.z(), q.w()}};
}

} // namespace
#endif

MmdPhysics::MmdPhysics(const PmxModel& model) : impl_(std::make_unique<Impl>()) {
    const auto finiteValues = [](const auto& values) {
        return std::ranges::all_of(values, [](const float value) { return std::isfinite(value); });
    };
    for (const auto& source : model.rigidBodies) {
        if (!finiteValues(source.size) || !finiteValues(source.position) || !finiteValues(source.rotation) ||
            !std::isfinite(source.mass) || !std::isfinite(source.linearDamping) ||
            !std::isfinite(source.angularDamping) || !std::isfinite(source.restitution) ||
            !std::isfinite(source.friction))
            throw std::runtime_error("PMX rigid body contains a non-finite numeric value");
    }
#if DAYO_HAS_BULLET
    impl_->collisionConfiguration = std::make_unique<btDefaultCollisionConfiguration>();
    impl_->dispatcher = std::make_unique<btCollisionDispatcher>(impl_->collisionConfiguration.get());
    impl_->broadphase = std::make_unique<btDbvtBroadphase>();
    impl_->solver = std::make_unique<btSequentialImpulseConstraintSolver>();
    impl_->world = std::make_unique<btDiscreteDynamicsWorld>(impl_->dispatcher.get(), impl_->broadphase.get(),
                                                             impl_->solver.get(), impl_->collisionConfiguration.get());
    impl_->world->setGravity(vector(impl_->gravity));
    impl_->shapes.reserve(model.rigidBodies.size());
    impl_->motionStates.reserve(model.rigidBodies.size());
    impl_->bodies.reserve(model.rigidBodies.size());
    impl_->initialTransforms.reserve(model.rigidBodies.size());
    impl_->kinematicStarts.reserve(model.rigidBodies.size());
    impl_->kinematicTargets.reserve(model.rigidBodies.size());
    impl_->kinematicDirty.reserve(model.rigidBodies.size());
    impl_->modes.reserve(model.rigidBodies.size());
    for (const auto& source : model.rigidBodies) {
        const auto validDimension = [](float value) { return std::isfinite(value) && std::abs(value) >= 0.001F; };
        bool invalidShape = !source.physicsEnabled;
        if (!invalidShape) {
            switch (source.shape) {
            case 0:
                invalidShape = !validDimension(source.size[0]);
                if (!invalidShape)
                    impl_->shapes.push_back(std::make_unique<btSphereShape>(std::abs(source.size[0])));
                break;
            case 1:
                invalidShape = !validDimension(source.size[0]) || !validDimension(source.size[1]) ||
                               !validDimension(source.size[2]);
                if (!invalidShape) {
                    const Float3 boxSize{std::abs(source.size[0]), std::abs(source.size[1]), std::abs(source.size[2])};
                    impl_->shapes.push_back(std::make_unique<btBoxShape>(vector(boxSize)));
                }
                break;
            case 2:
                invalidShape = !validDimension(source.size[0]) || !validDimension(source.size[1]);
                if (!invalidShape)
                    impl_->shapes.push_back(
                        std::make_unique<btCapsuleShape>(std::abs(source.size[0]), std::abs(source.size[1])));
                break;
            default:
                invalidShape = true;
                break;
            }
        }
        if (invalidShape)
            impl_->shapes.push_back(std::make_unique<btEmptyShape>());
        // Upstream MikuMikuDayo sets an explicit 0.01 margin on every rigid
        // shape instead of relying on the Bullet default.
        impl_->shapes.back()->setMargin(0.01F);
        const bool invalidTransform = !finite(source.position) || !finite(source.rotation);
        const auto initial =
            invalidTransform ? btTransform::getIdentity() : transform(source.position, source.rotation);
        impl_->initialTransforms.push_back(initial);
        impl_->kinematicStarts.push_back(initial);
        impl_->kinematicTargets.push_back(initial);
        impl_->kinematicDirty.push_back(0);
        impl_->motionStates.push_back(std::make_unique<btDefaultMotionState>(initial));
        // Some otherwise valid MMD models contain decorative rigid bodies with
        // invalid collision geometry. Bullet's btEmptyShape cannot compute
        // dynamic inertia, so keep those bodies as static placeholders. This
        // preserves the PMX body index mapping while avoiding an assertion in
        // btEmptyShape::calculateLocalInertia().
        const bool invalidMass = !std::isfinite(source.mass) || source.mass <= 0.0F;
        const btScalar candidateMass = (source.mode == 0 || invalidShape || invalidMass) ? 0.0F : source.mass;
        btVector3 inertia;
        inertia.setZero();
        if (candidateMass > 0.0F)
            impl_->shapes.back()->calculateLocalInertia(candidateMass, inertia);
        const bool unusableDynamicBody =
            candidateMass > 0.0F && (!std::isfinite(inertia.x()) || !std::isfinite(inertia.y()) ||
                                     !std::isfinite(inertia.z()) || inertia.length2() <= SIMD_EPSILON * SIMD_EPSILON);
        const bool dynamicUsable = candidateMass > 0.0F && !unusableDynamicBody && !invalidTransform;
        const btScalar effectiveMass = dynamicUsable ? candidateMass : 0.0F;
        const auto effectiveMode = dynamicUsable ? source.mode : std::uint8_t{0};
        if (!dynamicUsable)
            inertia.setZero();
        btRigidBody::btRigidBodyConstructionInfo info(effectiveMass, impl_->motionStates.back().get(),
                                                      impl_->shapes.back().get(), inertia);
        info.m_linearDamping = source.linearDamping;
        info.m_angularDamping = source.angularDamping;
        info.m_additionalDamping = true;
        info.m_restitution = source.restitution;
        info.m_friction = source.friction;
        impl_->bodies.push_back(std::make_unique<btRigidBody>(info));
        impl_->modes.push_back(effectiveMode);
        impl_->bodies.back()->setSleepingThresholds(0.01F, 0.1F * std::numbers::pi_v<float> / 180.0F);
        impl_->bodies.back()->setActivationState(DISABLE_DEACTIVATION);
        if (effectiveMode == 0) {
            impl_->bodies.back()->setCollisionFlags(impl_->bodies.back()->getCollisionFlags() |
                                                    btCollisionObject::CF_KINEMATIC_OBJECT);
        }
        const short group = static_cast<short>(1U << std::min<std::uint8_t>(source.group, 15));
        // Pass the PMX mask to Bullet unchanged, matching upstream
        // MikuMikuDayo (groupMask = r.passGroup). Real models are authored
        // for this convention: e.g. Tda-style legs use group 0 / mask
        // 0xffff (collide with everything) while skirt panels use groups
        // 14-15 / mask 0x3fff (collide with the body, not each other).
        // Inverting the mask here used to leave legs colliding with nothing
        // while enabling skirt self-collision instead.
        const short mask = static_cast<short>(source.collisionMask);
        impl_->world->addRigidBody(impl_->bodies.back().get(), group, mask);
    }
    impl_->constraints.reserve(model.joints.size());
    for (const auto& source : model.joints) {
        if (!source.physicsEnabled || source.type != 0 || source.bodyA < 0 || source.bodyB < 0 ||
            source.bodyA == source.bodyB || static_cast<std::size_t>(source.bodyA) >= impl_->bodies.size() ||
            static_cast<std::size_t>(source.bodyB) >= impl_->bodies.size())
            continue;
        const auto bodyAIndex = static_cast<std::size_t>(source.bodyA);
        const auto bodyBIndex = static_cast<std::size_t>(source.bodyB);
        if (!model.rigidBodies[bodyAIndex].physicsEnabled || !model.rigidBodies[bodyBIndex].physicsEnabled)
            continue;
        const auto& bodyA = impl_->bodies[bodyAIndex];
        const auto& bodyB = impl_->bodies[bodyBIndex];
        // Bullet cannot solve a 6DoF row when neither endpoint has inverse
        // mass. A number of PMX files contain decorative static-static joints.
        if (bodyA->getInvMass() <= 0.0F && bodyB->getInvMass() <= 0.0F)
            continue;
        if (!finite(source.position) || !finite(source.rotation) || !finite(source.translationMinimum) ||
            !finite(source.translationMaximum) || !finite(source.rotationMinimum) || !finite(source.rotationMaximum) ||
            !finite(source.translationSpring) || !finite(source.rotationSpring))
            continue;
        const auto jointWorld = transform(source.position, source.rotation);
        const auto frameA = impl_->initialTransforms[bodyAIndex].inverse() * jointWorld;
        const auto frameB = impl_->initialTransforms[bodyBIndex].inverse() * jointWorld;
        auto joint = std::make_unique<btGeneric6DofSpringConstraint>(*bodyA, *bodyB, frameA, frameB, true);
        joint->setLinearLowerLimit(vector(source.translationMinimum));
        joint->setLinearUpperLimit(vector(source.translationMaximum));
        joint->setAngularLowerLimit(vector(source.rotationMinimum));
        joint->setAngularUpperLimit(vector(source.rotationMaximum));
        for (int axis = 0; axis < 3; ++axis) {
            if (source.translationSpring[static_cast<std::size_t>(axis)] > 0.0F) {
                joint->enableSpring(axis, true);
                joint->setStiffness(axis, source.translationSpring[static_cast<std::size_t>(axis)]);
            }
            if (source.rotationSpring[static_cast<std::size_t>(axis)] > 0.0F) {
                joint->enableSpring(axis + 3, true);
                joint->setStiffness(axis + 3, source.rotationSpring[static_cast<std::size_t>(axis)]);
            }
        }
        joint->setEquilibriumPoint();
        // Upstream adds the constraint without disabling collision between
        // the linked bodies, so joint-connected pairs (e.g. hip to top
        // skirt row) still collide subject to their masks.
        impl_->world->addConstraint(joint.get());
        impl_->constraints.push_back(std::move(joint));
    }
#else
    static_cast<void>(model);
#endif
}

MmdPhysics::~MmdPhysics() = default;
MmdPhysics::MmdPhysics(MmdPhysics&&) noexcept = default;
MmdPhysics& MmdPhysics::operator=(MmdPhysics&&) noexcept = default;

SoftBodySimulation::SoftBodySimulation(const PmxModel& model)
    : initial_(model.vertices.size()), positions_(model.vertices.size()), velocities_(model.vertices.size()),
      pinned_(model.vertices.size()) {
    for (std::size_t index = 0; index < model.vertices.size(); ++index) {
        initial_[index] = model.vertices[index].position;
        positions_[index] = initial_[index];
    }
    for (const auto& softBody : model.softBodies) {
        if (softBody.material < 0 || static_cast<std::size_t>(softBody.material) >= model.materials.size())
            continue;
        std::size_t firstIndex = 0;
        for (std::size_t material = 0; material < static_cast<std::size_t>(softBody.material); ++material) {
            firstIndex += model.materials[material].indexCount;
        }
        if (firstIndex >= model.indices.size())
            continue;
        const auto materialIndexCount = model.materials[static_cast<std::size_t>(softBody.material)].indexCount;
        const auto lastIndex =
            firstIndex + std::min<std::size_t>(materialIndexCount, model.indices.size() - firstIndex);
        for (std::size_t index = firstIndex; index < lastIndex; ++index) {
            const auto vertex = model.indices[index];
            if (vertex < model.vertices.size())
                activeVertices_.push_back(vertex);
        }
        ++bodyCount_;
        for (const auto vertex : softBody.pinnedVertices) {
            if (vertex >= 0 && static_cast<std::size_t>(vertex) < pinned_.size())
                pinned_[static_cast<std::size_t>(vertex)] = 1;
        }
    }
    std::ranges::sort(activeVertices_);
    activeVertices_.erase(std::unique(activeVertices_.begin(), activeVertices_.end()), activeVertices_.end());
}

void SoftBodySimulation::reset() {
    positions_ = initial_;
    std::fill(velocities_.begin(), velocities_.end(), Float3{});
}

void SoftBodySimulation::step(float deltaSeconds, const Float3& gravity) {
    if (!available() || deltaSeconds <= 0.0F)
        return;
    const float dt = std::min(deltaSeconds, 0.05F);
    const float damping = std::pow(0.995F, dt * 60.0F);
    for (const auto vertex : activeVertices_) {
        const auto index = static_cast<std::size_t>(vertex);
        if (pinned_[index] != 0) {
            positions_[index] = initial_[index];
            velocities_[index] = {};
            continue;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto displacement = positions_[index][axis] - initial_[index][axis];
            velocities_[index][axis] += (gravity[axis] - displacement * 4.0F) * dt;
            velocities_[index][axis] *= damping;
            positions_[index][axis] += velocities_[index][axis] * dt;
        }
    }
}

void SoftBodySimulation::apply(std::span<PmxVertex> vertices) const {
    const auto count = std::min(vertices.size(), positions_.size());
    for (const auto vertex : activeVertices_) {
        const auto index = static_cast<std::size_t>(vertex);
        if (index >= count)
            continue;
        // The input vertices already contain VMD skinning and morph results.
        // Apply only the simulated displacement instead of restoring bind
        // positions over the animated mesh.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            vertices[index].position[axis] += positions_[index][axis] - initial_[index][axis];
        }
    }
}

bool MmdPhysics::available() const noexcept {
#if DAYO_HAS_BULLET
    return impl_ != nullptr && impl_->world != nullptr;
#else
    return false;
#endif
}

std::size_t MmdPhysics::bodyCount() const noexcept {
#if DAYO_HAS_BULLET
    return impl_->bodies.size();
#else
    return 0;
#endif
}

std::size_t MmdPhysics::jointCount() const noexcept {
#if DAYO_HAS_BULLET
    return impl_->constraints.size();
#else
    return 0;
#endif
}

std::uint8_t MmdPhysics::bodyMode(std::size_t body) const noexcept {
#if DAYO_HAS_BULLET
    return impl_ != nullptr && body < impl_->modes.size() ? impl_->modes[body] : std::uint8_t{0};
#else
    static_cast<void>(body);
    return 0;
#endif
}

void MmdPhysics::reset() {
#if DAYO_HAS_BULLET
    for (std::size_t i = 0; i < impl_->bodies.size(); ++i) {
        impl_->bodies[i]->setWorldTransform(impl_->initialTransforms[i]);
        impl_->bodies[i]->getMotionState()->setWorldTransform(impl_->initialTransforms[i]);
        impl_->bodies[i]->setInterpolationWorldTransform(impl_->initialTransforms[i]);
        impl_->bodies[i]->setLinearVelocity({0, 0, 0});
        impl_->bodies[i]->setAngularVelocity({0, 0, 0});
        impl_->bodies[i]->setInterpolationLinearVelocity({0, 0, 0});
        impl_->bodies[i]->setInterpolationAngularVelocity({0, 0, 0});
        impl_->bodies[i]->clearForces();
        impl_->bodies[i]->activate(true);
        impl_->kinematicStarts[i] = impl_->initialTransforms[i];
        impl_->kinematicTargets[i] = impl_->initialTransforms[i];
        impl_->kinematicDirty[i] = 0;
    }
    impl_->world->getBroadphase()->resetPool(impl_->dispatcher.get());
    impl_->world->getConstraintSolver()->reset();
    impl_->elapsed = 0.0F;
    impl_->world->setGravity(vector(impl_->gravity));
#endif
}

void MmdPhysics::step(float deltaSeconds) {
#if DAYO_HAS_BULLET
    if (deltaSeconds > 0.0F) {
        const float dt = std::min(deltaSeconds, 0.25F);
        impl_->elapsed += dt;
        auto gravity = impl_->gravity;
        if (impl_->gravityNoiseAmplitude > 0.0F && impl_->gravityNoiseFrequency > 0.0F) {
            gravity[1] += impl_->gravityNoiseAmplitude *
                          std::sin(impl_->elapsed * impl_->gravityNoiseFrequency * 2.0F * std::numbers::pi_v<float>);
        }
        impl_->world->setGravity(vector(gravity));
        constexpr float fixedStep = 1.0F / 120.0F;
        const auto substeps = std::max(1, static_cast<int>(std::ceil(dt / fixedStep)));
        const auto substep = dt / static_cast<float>(substeps);
        for (int step = 0; step < substeps; ++step) {
            const auto amount = static_cast<btScalar>(step + 1) / static_cast<btScalar>(substeps);
            for (std::size_t body = 0; body < impl_->bodies.size(); ++body) {
                if (impl_->modes[body] != 0 || impl_->kinematicDirty[body] == 0)
                    continue;
                const auto world = interpolate(impl_->kinematicStarts[body], impl_->kinematicTargets[body], amount);
                impl_->bodies[body]->setWorldTransform(world);
                impl_->bodies[body]->getMotionState()->setWorldTransform(world);
                impl_->bodies[body]->setInterpolationWorldTransform(world);
                impl_->world->updateSingleAabb(impl_->bodies[body].get());
            }
            impl_->world->stepSimulation(substep, 0, substep);
        }
        for (std::size_t body = 0; body < impl_->bodies.size(); ++body) {
            if (impl_->kinematicDirty[body] == 0)
                continue;
            impl_->bodies[body]->setWorldTransform(impl_->kinematicTargets[body]);
            impl_->bodies[body]->getMotionState()->setWorldTransform(impl_->kinematicTargets[body]);
            impl_->bodies[body]->setInterpolationWorldTransform(impl_->kinematicTargets[body]);
            impl_->kinematicStarts[body] = impl_->kinematicTargets[body];
            impl_->kinematicDirty[body] = 0;
        }
    }
#else
    static_cast<void>(deltaSeconds);
#endif
}

void MmdPhysics::setGravity(const Float3& gravity) {
#if DAYO_HAS_BULLET
    impl_->gravity = gravity;
    impl_->world->setGravity(vector(gravity));
#else
    static_cast<void>(gravity);
#endif
}

void MmdPhysics::setGravityNoise(float amplitude, float frequency) {
#if DAYO_HAS_BULLET
    impl_->gravityNoiseAmplitude = std::max(amplitude, 0.0F);
    impl_->gravityNoiseFrequency = std::max(frequency, 0.0F);
#else
    static_cast<void>(amplitude);
    static_cast<void>(frequency);
#endif
}

void MmdPhysics::setFloorCollision(bool enabled) {
#if DAYO_HAS_BULLET
    if (impl_->floorCollision == enabled)
        return;
    impl_->floorCollision = enabled;
    if (!enabled) {
        if (impl_->floorBody != nullptr)
            impl_->world->removeRigidBody(impl_->floorBody.get());
        impl_->floorBody.reset();
        impl_->floorMotionState.reset();
        impl_->floorShape.reset();
        return;
    }
    impl_->floorShape = std::make_unique<btStaticPlaneShape>(btVector3(0.0F, 1.0F, 0.0F), 0.0F);
    impl_->floorMotionState = std::make_unique<btDefaultMotionState>(btTransform::getIdentity());
    btRigidBody::btRigidBodyConstructionInfo info(0.0F, impl_->floorMotionState.get(), impl_->floorShape.get());
    impl_->floorBody = std::make_unique<btRigidBody>(info);
    impl_->world->addRigidBody(impl_->floorBody.get(), 1, -1);
#else
    static_cast<void>(enabled);
#endif
}

void MmdPhysics::setKinematicTransform(std::size_t body, const PhysicsTransform& value) {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size())
        throw std::out_of_range("PMX rigid body index");
    if (impl_->modes[body] != 0 || !finite(value))
        return;
    const auto world = transform(value);
    if (impl_->kinematicDirty[body] == 0) {
        impl_->kinematicStarts[body] = impl_->kinematicTargets[body];
        impl_->kinematicDirty[body] = 1;
    }
    impl_->kinematicTargets[body] = world;
#else
    static_cast<void>(body);
    static_cast<void>(value);
#endif
}

void MmdPhysics::teleportBody(std::size_t body, const PhysicsTransform& value) {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size())
        throw std::out_of_range("PMX rigid body index");
    if (!finite(value))
        return;
    const auto world = transform(value);
    auto& rigidBody = impl_->bodies[body];
    rigidBody->setWorldTransform(world);
    rigidBody->getMotionState()->setWorldTransform(world);
    rigidBody->setInterpolationWorldTransform(world);
    rigidBody->setLinearVelocity({0.0F, 0.0F, 0.0F});
    rigidBody->setAngularVelocity({0.0F, 0.0F, 0.0F});
    rigidBody->setInterpolationLinearVelocity({0.0F, 0.0F, 0.0F});
    rigidBody->setInterpolationAngularVelocity({0.0F, 0.0F, 0.0F});
    rigidBody->clearForces();
    rigidBody->activate(true);
    impl_->world->updateSingleAabb(rigidBody.get());
    impl_->kinematicStarts[body] = world;
    impl_->kinematicTargets[body] = world;
    impl_->kinematicDirty[body] = 0;
#else
    static_cast<void>(body);
    static_cast<void>(value);
#endif
}

void MmdPhysics::applyImpulse(std::size_t body, const Float3& linear, const Float3& angular, bool local) {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size())
        throw std::out_of_range("PMX rigid body index");
    auto linearValue = vector(linear);
    auto angularValue = vector(angular);
    if (local) {
        const auto& basis = impl_->bodies[body]->getWorldTransform().getBasis();
        linearValue = basis * linearValue;
        angularValue = basis * angularValue;
    }
    impl_->bodies[body]->applyCentralImpulse(linearValue);
    impl_->bodies[body]->applyTorqueImpulse(angularValue);
    impl_->bodies[body]->activate(true);
#else
    static_cast<void>(body);
    static_cast<void>(linear);
    static_cast<void>(angular);
    static_cast<void>(local);
#endif
}

void MmdPhysics::clearMotion(std::size_t body) {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size())
        throw std::out_of_range("PMX rigid body index");
    impl_->bodies[body]->setLinearVelocity({0.0F, 0.0F, 0.0F});
    impl_->bodies[body]->setAngularVelocity({0.0F, 0.0F, 0.0F});
    impl_->bodies[body]->setInterpolationLinearVelocity({0.0F, 0.0F, 0.0F});
    impl_->bodies[body]->setInterpolationAngularVelocity({0.0F, 0.0F, 0.0F});
    impl_->bodies[body]->clearForces();
    impl_->bodies[body]->activate(true);
#else
    static_cast<void>(body);
#endif
}

PhysicsTransform MmdPhysics::bodyTransform(std::size_t body) const {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size())
        throw std::out_of_range("PMX rigid body index");
    btTransform value;
    impl_->bodies[body]->getMotionState()->getWorldTransform(value);
    return transform(value);
#else
    static_cast<void>(body);
    return {};
#endif
}

} // namespace dayo::core
