#include "g4go/optical/Geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace G4GO::Optical {

namespace {

constexpr auto kPi{3.14159265358979323846F};
constexpr auto kTwoPi{2.0F * kPi};

auto Add(Vector3 left, Vector3 right) -> Vector3 {
    return {left.fX + right.fX, left.fY + right.fY, left.fZ + right.fZ};
}

auto Subtract(Vector3 left, Vector3 right) -> Vector3 {
    return {left.fX - right.fX, left.fY - right.fY, left.fZ - right.fZ};
}

auto Scale(Vector3 vector, float value) -> Vector3 {
    return {vector.fX * value, vector.fY * value, vector.fZ * value};
}

auto Dot(Vector3 left, Vector3 right) -> float {
    return left.fX * right.fX + left.fY * right.fY + left.fZ * right.fZ;
}

auto LengthSquared(Vector3 vector) -> float { return Dot(vector, vector); }

auto Normalize(Vector3 vector) -> Vector3 {
    const auto lengthSquared{LengthSquared(vector)};
    if (lengthSquared <= 0.0F) {
        return {};
    }
    return Scale(vector, 1.0F / std::sqrt(lengthSquared));
}

auto PhiInRange(float phi, float startPhi, float deltaPhi) -> bool {
    if (deltaPhi >= kTwoPi - 1.0e-5F) {
        return true;
    }
    auto relative{std::fmod(phi - startPhi, kTwoPi)};
    if (relative < 0.0F) {
        relative += kTwoPi;
    }
    return relative <= deltaPhi;
}

auto ContainsBox(const Solid& solid, Vector3 point) -> bool {
    const auto local{ToLocal(solid.fTransform, point)};
    constexpr auto tolerance{1.0e-5F};
    return std::abs(local.fX) <= solid.fHalfSizeMm.fX + tolerance &&
           std::abs(local.fY) <= solid.fHalfSizeMm.fY + tolerance &&
           std::abs(local.fZ) <= solid.fHalfSizeMm.fZ + tolerance;
}

auto ContainsTub(const Solid& solid, Vector3 point) -> bool {
    const auto local{ToLocal(solid.fTransform, point)};
    constexpr auto tolerance{1.0e-5F};
    const auto radiusSquared{local.fX * local.fX + local.fY * local.fY};
    const auto phi{std::atan2(local.fY, local.fX)};
    return std::abs(local.fZ) <= solid.fHalfLengthMm + tolerance &&
           radiusSquared >=
               solid.fInnerRadiusMm * solid.fInnerRadiusMm - tolerance &&
           radiusSquared <=
               solid.fOuterRadiusMm * solid.fOuterRadiusMm + tolerance &&
           PhiInRange(phi, solid.fStartPhi, solid.fDeltaPhi);
}

auto IntersectBox(const Solid& solid,
                  const Photon& photon,
                  float epsilonMm) -> std::optional<Intersection> {
    const auto origin{ToLocal(
        solid.fTransform,
        {photon.fPositionMm.fX, photon.fPositionMm.fY, photon.fPositionMm.fZ})};
    const auto direction{RotateToLocal(
        solid.fTransform.fRotation,
        {photon.fDirection.fX, photon.fDirection.fY, photon.fDirection.fZ})};

    auto tMin{-std::numeric_limits<float>::infinity()};
    auto tMax{std::numeric_limits<float>::infinity()};
    auto nearAxis{0};
    auto farAxis{0};
    const std::array halfSize{solid.fHalfSizeMm.fX, solid.fHalfSizeMm.fY,
                              solid.fHalfSizeMm.fZ};
    const std::array position{origin.fX, origin.fY, origin.fZ};
    const std::array component{direction.fX, direction.fY, direction.fZ};

    for (auto axis{0}; axis < 3; ++axis) {
        if (std::abs(component[axis]) < 1.0e-8F) {
            if (std::abs(position[axis]) > halfSize[axis]) {
                return {};
            }
            continue;
        }

        auto first{(-halfSize[axis] - position[axis]) / component[axis]};
        auto second{(halfSize[axis] - position[axis]) / component[axis]};
        auto firstNormalAxis{axis};
        auto secondNormalAxis{axis};
        if (first > second) {
            std::swap(first, second);
            std::swap(firstNormalAxis, secondNormalAxis);
        }
        if (first > tMin) {
            tMin = first;
            nearAxis = firstNormalAxis;
        }
        if (second < tMax) {
            tMax = second;
            farAxis = secondNormalAxis;
        }
        if (tMin > tMax) {
            return {};
        }
    }

    const auto distance{tMin > epsilonMm ? tMin : tMax};
    if (!std::isfinite(distance) || distance <= epsilonMm) {
        return {};
    }

    const auto axis{tMin > epsilonMm ? nearAxis : farAxis};
    Vector3 localNormal{};
    const auto sign{component[axis] > 0.0F ? -1.0F : 1.0F};
    if (axis == 0) {
        localNormal.fX = sign;
    } else if (axis == 1) {
        localNormal.fY = sign;
    } else {
        localNormal.fZ = sign;
    }
    return Intersection{distance,
                        RotateToWorld(solid.fTransform.fRotation, localNormal),
                        solid.fVolumeID};
}

auto AddCandidate(std::vector<std::pair<float, Vector3>>& candidates,
                  float distance,
                  Vector3 normal,
                  float epsilonMm) -> void {
    if (std::isfinite(distance) && distance > epsilonMm) {
        candidates.emplace_back(distance, normal);
    }
}

auto IntersectTub(const Solid& solid,
                  const Photon& photon,
                  float epsilonMm) -> std::optional<Intersection> {
    const auto origin{ToLocal(
        solid.fTransform,
        {photon.fPositionMm.fX, photon.fPositionMm.fY, photon.fPositionMm.fZ})};
    const auto direction{RotateToLocal(
        solid.fTransform.fRotation,
        {photon.fDirection.fX, photon.fDirection.fY, photon.fDirection.fZ})};
    std::vector<std::pair<float, Vector3>> candidates{};

    const auto addCylinder{[&](float radius, bool inner) {
        if (radius <= 0.0F) {
            return;
        }
        const auto a{direction.fX * direction.fX +
                     direction.fY * direction.fY};
        const auto b{2.0F *
                     (origin.fX * direction.fX + origin.fY * direction.fY)};
        const auto c{origin.fX * origin.fX + origin.fY * origin.fY -
                     radius * radius};
        if (std::abs(a) < 1.0e-8F) {
            return;
        }
        const auto discriminant{b * b - 4.0F * a * c};
        if (discriminant < 0.0F) {
            return;
        }
        const auto root{std::sqrt(std::max(discriminant, 0.0F))};
        const auto first{(-b - root) / (2.0F * a)};
        const auto second{(-b + root) / (2.0F * a)};
        const auto addRoot{[&](float distance) {
            const auto z{origin.fZ + distance * direction.fZ};
            if (z < -solid.fHalfLengthMm - epsilonMm ||
                z > solid.fHalfLengthMm + epsilonMm) {
                return;
            }
            const auto point{Add(origin, Scale(direction, distance))};
            const auto radialPhi{std::atan2(point.fY, point.fX)};
            if (!PhiInRange(radialPhi, solid.fStartPhi, solid.fDeltaPhi)) {
                return;
            }
            const auto radial{Normalize({point.fX, point.fY, 0.0F})};
            AddCandidate(candidates, distance,
                         inner ? Scale(radial, -1.0F) : radial, epsilonMm);
        }};
        addRoot(first);
        addRoot(second);
    }};

    addCylinder(solid.fOuterRadiusMm, false);
    addCylinder(solid.fInnerRadiusMm, true);

    if (std::abs(direction.fZ) >= 1.0e-8F) {
        const auto addCap{[&](float z, float normalZ) {
            const auto distance{(z - origin.fZ) / direction.fZ};
            if (distance <= epsilonMm) {
                return;
            }
            const auto point{Add(origin, Scale(direction, distance))};
            const auto radialSquared{point.fX * point.fX + point.fY * point.fY};
            if (radialSquared >=
                    solid.fInnerRadiusMm * solid.fInnerRadiusMm - epsilonMm &&
                radialSquared <=
                    solid.fOuterRadiusMm * solid.fOuterRadiusMm + epsilonMm) {
                AddCandidate(candidates, distance, {0.0F, 0.0F, normalZ},
                             epsilonMm);
            }
        }};
        addCap(solid.fHalfLengthMm, 1.0F);
        addCap(-solid.fHalfLengthMm, -1.0F);
    }

    if (candidates.empty()) {
        return {};
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });
    const auto& candidate{candidates.front()};
    return Intersection{
        candidate.first,
        RotateToWorld(solid.fTransform.fRotation, candidate.second),
        solid.fVolumeID,
    };
}

} // namespace

auto ToWorld(const Transform& transform, Vector3 point) -> Vector3 {
    return Add(RotateToWorld(transform.fRotation, point),
               transform.fTranslationMm);
}

auto ToLocal(const Transform& transform, Vector3 point) -> Vector3 {
    return RotateToLocal(transform.fRotation,
                         Subtract(point, transform.fTranslationMm));
}

auto RotateToWorld(const Rotation& rotation, Vector3 vector) -> Vector3 {
    return {
        rotation.fXX * vector.fX + rotation.fXY * vector.fY +
            rotation.fXZ * vector.fZ,
        rotation.fYX * vector.fX + rotation.fYY * vector.fY +
            rotation.fYZ * vector.fZ,
        rotation.fZX * vector.fX + rotation.fZY * vector.fY +
            rotation.fZZ * vector.fZ,
    };
}

auto RotateToLocal(const Rotation& rotation, Vector3 vector) -> Vector3 {
    return {
        rotation.fXX * vector.fX + rotation.fYX * vector.fY +
            rotation.fZX * vector.fZ,
        rotation.fXY * vector.fX + rotation.fYY * vector.fY +
            rotation.fZY * vector.fZ,
        rotation.fXZ * vector.fX + rotation.fYZ * vector.fY +
            rotation.fZZ * vector.fZ,
    };
}

auto Contains(const Solid& solid, Vector3 point) -> bool {
    if (solid.fKind == SolidKind::Box) {
        return ContainsBox(solid, point);
    }
    return ContainsTub(solid, point);
}

auto Intersect(const Solid& solid,
               const Photon& photon,
               float epsilonMm) -> std::optional<Intersection> {
    if (solid.fKind == SolidKind::Box) {
        return IntersectBox(solid, photon, epsilonMm);
    }
    return IntersectTub(solid, photon, epsilonMm);
}

auto LocateVolume(const Scene& scene, Vector3 point) -> std::uint32_t {
    const Solid* selected{nullptr};
    for (const auto& solid : scene.Solids()) {
        if (!Contains(solid, point)) {
            continue;
        }
        if (selected == nullptr || solid.fDepth > selected->fDepth ||
            (solid.fDepth == selected->fDepth &&
             solid.fVolumeID < selected->fVolumeID)) {
            selected = &solid;
        }
    }
    return selected == nullptr ? InvalidID : selected->fVolumeID;
}

auto NextIntersection(const Scene& scene,
                      const Photon& photon,
                      float epsilonMm) -> std::optional<Intersection> {
    std::optional<Intersection> nearest{};
    for (const auto& solid : scene.Solids()) {
        const auto intersection{Intersect(solid, photon, epsilonMm)};
        if (!intersection ||
            (nearest && intersection->fDistanceMm >= nearest->fDistanceMm)) {
            continue;
        }
        nearest = intersection;
    }
    return nearest;
}

} // namespace G4GO::Optical
