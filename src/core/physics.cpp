#include "core/physics.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
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
    std::vector<std::uint8_t> modes;
    std::unique_ptr<btStaticPlaneShape> floorShape;
    std::unique_ptr<btDefaultMotionState> floorMotionState;
    std::unique_ptr<btRigidBody> floorBody;
    Float3 gravity { 0.0F, -9.8F, 0.0F };
    float gravityNoiseAmplitude {};
    float gravityNoiseFrequency {};
    float elapsed {};
    bool floorCollision {};

    ~Impl() {
        if (world == nullptr) return;
        for (auto& constraint : constraints) world->removeConstraint(constraint.get());
        if (floorBody != nullptr) world->removeRigidBody(floorBody.get());
        for (auto& body : bodies) world->removeRigidBody(body.get());
    }
#endif
};

#if DAYO_HAS_BULLET
namespace {

btVector3 vector(const Float3& value) { return { value[0], value[1], value[2] }; }
Float3 vector(const btVector3& value) { return { value.x(), value.y(), value.z() }; }

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

bool finite(const PhysicsTransform& value) {
    return std::ranges::all_of(value.position, [](float component) { return std::isfinite(component); })
        && std::ranges::all_of(value.rotation, [](float component) { return std::isfinite(component); });
}

PhysicsTransform transform(const btTransform& value) {
    const auto& q = value.getRotation();
    return { vector(value.getOrigin()), { q.x(), q.y(), q.z(), q.w() } };
}

} // namespace
#endif

MmdPhysics::MmdPhysics(const PmxModel& model) : impl_(std::make_unique<Impl>()) {
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
    impl_->modes.reserve(model.rigidBodies.size());
    for (const auto& source : model.rigidBodies) {
        const Float3 safeSize {
            std::max(std::abs(source.size[0]), 0.001F),
            std::max(std::abs(source.size[1]), 0.001F),
            std::max(std::abs(source.size[2]), 0.001F),
        };
        const bool degenerate = !std::ranges::all_of(source.size, [](float component) {
            return std::isfinite(component) && std::abs(component) >= 0.001F;
        });
        if (degenerate) {
            impl_->shapes.push_back(std::make_unique<btEmptyShape>());
        } else {
        switch (source.shape) {
        case 0: impl_->shapes.push_back(std::make_unique<btSphereShape>(safeSize[0])); break;
        case 1: impl_->shapes.push_back(std::make_unique<btBoxShape>(vector(safeSize))); break;
        case 2: impl_->shapes.push_back(std::make_unique<btCapsuleShape>(safeSize[0], safeSize[1])); break;
        default: throw std::runtime_error("unsupported PMX rigid body shape");
        }
        }
        const auto initial = transform(source.position, source.rotation);
        impl_->initialTransforms.push_back(initial);
        impl_->motionStates.push_back(std::make_unique<btDefaultMotionState>(initial));
        // Some otherwise valid MMD models contain decorative rigid bodies with
        // zero-sized collision geometry. Bullet's btEmptyShape cannot compute
        // dynamic inertia, so keep those bodies as static placeholders. This
        // preserves the PMX body index mapping while avoiding an assertion in
        // btEmptyShape::calculateLocalInertia().
        const btScalar mass = (source.mode == 0 || degenerate) ? 0.0F : source.mass;
        btVector3 inertia {};
        if (mass > 0.0F) impl_->shapes.back()->calculateLocalInertia(mass, inertia);
        btRigidBody::btRigidBodyConstructionInfo info(mass, impl_->motionStates.back().get(),
                                                      impl_->shapes.back().get(), inertia);
        info.m_linearDamping = source.linearDamping;
        info.m_angularDamping = source.angularDamping;
        info.m_restitution = source.restitution;
        info.m_friction = source.friction;
        impl_->bodies.push_back(std::make_unique<btRigidBody>(info));
        impl_->modes.push_back(source.mode);
        if (source.mode == 0) {
            impl_->bodies.back()->setCollisionFlags(impl_->bodies.back()->getCollisionFlags()
                                                    | btCollisionObject::CF_KINEMATIC_OBJECT);
            impl_->bodies.back()->setActivationState(DISABLE_DEACTIVATION);
        }
        const short group = static_cast<short>(1U << std::min<std::uint8_t>(source.group, 15));
        const short mask = static_cast<short>(source.collisionMask);
        impl_->world->addRigidBody(impl_->bodies.back().get(), group, mask);
    }
    impl_->constraints.reserve(model.joints.size());
    for (const auto& source : model.joints) {
        if (source.type != 0 || source.bodyA < 0 || source.bodyB < 0 || source.bodyA == source.bodyB
            || static_cast<std::size_t>(source.bodyA) >= impl_->bodies.size()
            || static_cast<std::size_t>(source.bodyB) >= impl_->bodies.size()) continue;
        const auto& bodyA = impl_->bodies[static_cast<std::size_t>(source.bodyA)];
        const auto& bodyB = impl_->bodies[static_cast<std::size_t>(source.bodyB)];
        // Bullet cannot solve a 6DoF row when neither endpoint has inverse
        // mass. A number of PMX files contain decorative static-static joints.
        if (bodyA->getInvMass() <= 0.0F && bodyB->getInvMass() <= 0.0F) continue;
        const auto finite3 = [](const Float3& value) {
            return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
        };
        if (!finite3(source.position) || !finite3(source.rotation)
            || !finite3(source.translationMinimum) || !finite3(source.translationMaximum)
            || !finite3(source.rotationMinimum) || !finite3(source.rotationMaximum)
            || !finite3(source.translationSpring) || !finite3(source.rotationSpring)) continue;
        const auto jointWorld = transform(source.position, source.rotation);
        const auto frameA = impl_->initialTransforms[static_cast<std::size_t>(source.bodyA)].inverse() * jointWorld;
        const auto frameB = impl_->initialTransforms[static_cast<std::size_t>(source.bodyB)].inverse() * jointWorld;
        auto joint = std::make_unique<btGeneric6DofSpringConstraint>(
            *bodyA, *bodyB, frameA, frameB, true);
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
        impl_->world->addConstraint(joint.get(), true);
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
    : initial_(model.vertices.size()), positions_(model.vertices.size()),
      velocities_(model.vertices.size()), pinned_(model.vertices.size()), bodyCount_(model.softBodies.size()) {
    for (std::size_t index = 0; index < model.vertices.size(); ++index) {
        initial_[index] = model.vertices[index].position;
        positions_[index] = initial_[index];
    }
    for (const auto& softBody : model.softBodies) {
        for (const auto vertex : softBody.pinnedVertices) {
            if (vertex >= 0 && static_cast<std::size_t>(vertex) < pinned_.size()) pinned_[static_cast<std::size_t>(vertex)] = 1;
        }
    }
}

void SoftBodySimulation::reset() {
    positions_ = initial_;
    std::fill(velocities_.begin(), velocities_.end(), Float3 {});
}

void SoftBodySimulation::step(float deltaSeconds, const Float3& gravity) {
    if (!available() || deltaSeconds <= 0.0F) return;
    const float dt = std::min(deltaSeconds, 0.05F);
    for (std::size_t index = 0; index < positions_.size(); ++index) {
        if (pinned_[index] != 0) { positions_[index] = initial_[index]; velocities_[index] = {}; continue; }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            velocities_[index][axis] += gravity[axis] * dt;
            velocities_[index][axis] *= 0.995F;
            positions_[index][axis] += velocities_[index][axis] * dt;
        }
    }
}

void SoftBodySimulation::apply(std::span<PmxVertex> vertices) const {
    const auto count = std::min(vertices.size(), positions_.size());
    for (std::size_t index = 0; index < count; ++index) vertices[index].position = positions_[index];
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

void MmdPhysics::reset() {
#if DAYO_HAS_BULLET
    for (std::size_t i = 0; i < impl_->bodies.size(); ++i) {
        impl_->bodies[i]->setWorldTransform(impl_->initialTransforms[i]);
        impl_->bodies[i]->getMotionState()->setWorldTransform(impl_->initialTransforms[i]);
        impl_->bodies[i]->setLinearVelocity({ 0, 0, 0 });
        impl_->bodies[i]->setAngularVelocity({ 0, 0, 0 });
        impl_->bodies[i]->clearForces();
        impl_->bodies[i]->activate(true);
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
            gravity[1] += impl_->gravityNoiseAmplitude
                * std::sin(impl_->elapsed * impl_->gravityNoiseFrequency * 2.0F * std::numbers::pi_v<float>);
        }
        impl_->world->setGravity(vector(gravity));
        impl_->world->stepSimulation(dt, 10, 1.0F / 120.0F);
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
    static_cast<void>(amplitude); static_cast<void>(frequency);
#endif
}

void MmdPhysics::setFloorCollision(bool enabled) {
#if DAYO_HAS_BULLET
    if (impl_->floorCollision == enabled) return;
    impl_->floorCollision = enabled;
    if (!enabled) {
        if (impl_->floorBody != nullptr) impl_->world->removeRigidBody(impl_->floorBody.get());
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
    if (body >= impl_->bodies.size()) throw std::out_of_range("PMX rigid body index");
    if (impl_->modes[body] != 0 || !finite(value)) return;
    const auto world = transform(value);
    impl_->bodies[body]->setWorldTransform(world);
    impl_->bodies[body]->getMotionState()->setWorldTransform(world);
#else
    static_cast<void>(body); static_cast<void>(value);
#endif
}

void MmdPhysics::applyImpulse(std::size_t body, const Float3& linear, const Float3& angular, bool local) {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size()) throw std::out_of_range("PMX rigid body index");
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
    static_cast<void>(body); static_cast<void>(linear); static_cast<void>(angular); static_cast<void>(local);
#endif
}

void MmdPhysics::clearMotion(std::size_t body) {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size()) throw std::out_of_range("PMX rigid body index");
    impl_->bodies[body]->setLinearVelocity({ 0.0F, 0.0F, 0.0F });
    impl_->bodies[body]->setAngularVelocity({ 0.0F, 0.0F, 0.0F });
    impl_->bodies[body]->setInterpolationLinearVelocity({ 0.0F, 0.0F, 0.0F });
    impl_->bodies[body]->setInterpolationAngularVelocity({ 0.0F, 0.0F, 0.0F });
    impl_->bodies[body]->clearForces();
    impl_->bodies[body]->activate(true);
#else
    static_cast<void>(body);
#endif
}

PhysicsTransform MmdPhysics::bodyTransform(std::size_t body) const {
#if DAYO_HAS_BULLET
    if (body >= impl_->bodies.size()) throw std::out_of_range("PMX rigid body index");
    btTransform value;
    impl_->bodies[body]->getMotionState()->getWorldTransform(value);
    return transform(value);
#else
    static_cast<void>(body);
    return {};
#endif
}

} // namespace dayo::core
