#pragma once

#include "core/model_probe.hpp"

#include <mmd/vmd.hpp>

// Transitional compatibility facade. New application code should use mmd::*.
namespace dayo::core {
using mmd::catmullRom;
using mmd::decodeCp932;
using mmd::encodeCp932;
using mmd::evaluateCamera;
using mmd::evaluateLight;
using mmd::InterpolationMode;
using mmd::loadVmd;
using mmd::loadVpd;
using mmd::MotionDocument;
using mmd::saveVmd;
using mmd::toMotionDocument;
using mmd::toVmdMotion;
using mmd::VmdayoExternalParentKey;
using mmd::VmdayoGravityKey;
using mmd::VmdBoneKey;
using mmd::VmdCameraKey;
using mmd::VmdCameraState;
using mmd::VmdIkKey;
using mmd::VmdIkState;
using mmd::VmdLightKey;
using mmd::VmdMorphKey;
using mmd::VmdMotion;
using mmd::VmdShadowKey;
using mmd::VpdBonePose;
using mmd::VpdPose;
} // namespace dayo::core
