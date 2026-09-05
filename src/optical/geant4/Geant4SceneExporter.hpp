#pragma once

#include "g4go/optical/Scene.hpp"

#include <cstdint>

class G4VPhysicalVolume;

namespace G4GO::Optical {

class Geant4SceneExporter final {
public:
    explicit Geant4SceneExporter(std::uint32_t meshRotationSteps = 360) :
        fMeshRotationSteps{meshRotationSteps} {}
    ~Geant4SceneExporter() = default;

    auto Export(const G4VPhysicalVolume* world) const -> Scene;

private:
    std::uint32_t fMeshRotationSteps;
};

} // namespace G4GO::Optical
