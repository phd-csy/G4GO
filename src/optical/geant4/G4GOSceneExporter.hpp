#pragma once

#include "g4go/optical/Scene.hpp"

#include <cstdint>

class G4VPhysicalVolume;

namespace G4GO::Optical {

class G4GOSceneExporter final {
public:
    explicit G4GOSceneExporter(std::uint32_t meshRotationSteps = 360) :
        fMeshRotationSteps{meshRotationSteps} {}
    ~G4GOSceneExporter() = default;

    auto Export(const G4VPhysicalVolume* world) const -> Scene;

private:
    std::uint32_t fMeshRotationSteps;
};

} // namespace G4GO::Optical
