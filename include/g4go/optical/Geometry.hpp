#pragma once

#include "g4go/optical/Scene.hpp"

namespace G4GO::Optical {

auto ToWorld(const Transform& transform, Vector3 point) -> Vector3;
auto ToLocal(const Transform& transform, Vector3 point) -> Vector3;
auto RotateToWorld(const Rotation& rotation, Vector3 vector) -> Vector3;
auto RotateToLocal(const Rotation& rotation, Vector3 vector) -> Vector3;

} // namespace G4GO::Optical
