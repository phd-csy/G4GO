#pragma once

#include "g4go/optical/Scene.hpp"

class G4VPhysicalVolume;

namespace G4GO::Optical {

class Geant4SceneExporter final {
public:
    Geant4SceneExporter() = default;
    ~Geant4SceneExporter() = default;

    auto Export(const G4VPhysicalVolume* world) const -> Scene;
};

} // namespace G4GO::Optical
