#pragma once

#include "G4ThreeVector.hh"
#include "g4go/optical/Scene.hpp"

namespace G4GO::Optical {

class GeometryTransformer final {
public:
    GeometryTransformer() = delete;

    static auto ToWorld(const Transform& transform, G4ThreeVector point)
        -> G4ThreeVector;
    static auto ToLocal(const Transform& transform, G4ThreeVector point)
        -> G4ThreeVector;
    static auto RotateToWorld(const Rotation& rotation, G4ThreeVector vector)
        -> G4ThreeVector;
    static auto RotateToLocal(const Rotation& rotation, G4ThreeVector vector)
        -> G4ThreeVector;
};

} // namespace G4GO::Optical
