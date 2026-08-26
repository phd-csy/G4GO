#include "g4go/optical/GeometryTransformer.hpp"

#include "G4ThreeVector.hh"

namespace G4GO::Optical {

namespace {

auto Add(const G4ThreeVector& left, const G4ThreeVector& right)
    -> G4ThreeVector {
    return {left.x() + right.x(), left.y() + right.y(), left.z() + right.z()};
}

auto Subtract(const G4ThreeVector& left, const G4ThreeVector& right)
    -> G4ThreeVector {
    return {left.x() - right.x(), left.y() - right.y(), left.z() - right.z()};
}

} // namespace

auto GeometryTransformer::ToWorld(const Transform& transform,
                                  G4ThreeVector point) -> G4ThreeVector {
    return Add(RotateToWorld(transform.fRotation, point),
               transform.fTranslationMm);
}

auto GeometryTransformer::ToLocal(const Transform& transform,
                                  G4ThreeVector point) -> G4ThreeVector {
    return RotateToLocal(transform.fRotation,
                         Subtract(point, transform.fTranslationMm));
}

auto GeometryTransformer::RotateToWorld(const Rotation& rotation,
                                        G4ThreeVector vector)
    -> G4ThreeVector {
    return {
        rotation.fXX * vector.x() + rotation.fXY * vector.y() +
            rotation.fXZ * vector.z(),
        rotation.fYX * vector.x() + rotation.fYY * vector.y() +
            rotation.fYZ * vector.z(),
        rotation.fZX * vector.x() + rotation.fZY * vector.y() +
            rotation.fZZ * vector.z(),
    };
}

auto GeometryTransformer::RotateToLocal(const Rotation& rotation,
                                        G4ThreeVector vector)
    -> G4ThreeVector {
    return {
        rotation.fXX * vector.x() + rotation.fYX * vector.y() +
            rotation.fZX * vector.z(),
        rotation.fXY * vector.x() + rotation.fYY * vector.y() +
            rotation.fZY * vector.z(),
        rotation.fXZ * vector.x() + rotation.fYZ * vector.y() +
            rotation.fZZ * vector.z(),
    };
}

} // namespace G4GO::Optical
