#include "g4go/optical/Geometry.hpp"

namespace G4GO::Optical {

namespace {

auto Add(Vector3 left, Vector3 right) -> Vector3 {
    return {left.fX + right.fX, left.fY + right.fY, left.fZ + right.fZ};
}

auto Subtract(Vector3 left, Vector3 right) -> Vector3 {
    return {left.fX - right.fX, left.fY - right.fY, left.fZ - right.fZ};
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

} // namespace G4GO::Optical
