#pragma once

#include "g4go/optical/Scene.hpp"

#include <optional>

namespace G4GO::Optical {

struct Intersection {
    float fDistanceMm{};
    Vector3 fNormal{};
    std::uint32_t fVolumeID{InvalidID};
};

auto ToWorld(const Transform& transform, Vector3 point) -> Vector3;
auto ToLocal(const Transform& transform, Vector3 point) -> Vector3;
auto RotateToWorld(const Rotation& rotation, Vector3 vector) -> Vector3;
auto RotateToLocal(const Rotation& rotation, Vector3 vector) -> Vector3;

auto Contains(const Solid& solid, Vector3 point) -> bool;
auto Intersect(const Solid& solid,
               const Photon& photon,
               float epsilonMm) -> std::optional<Intersection>;
auto LocateVolume(const Scene& scene, Vector3 point) -> std::uint32_t;
auto NextIntersection(const Scene& scene,
                      const Photon& photon,
                      float epsilonMm) -> std::optional<Intersection>;

} // namespace G4GO::Optical
