#ifndef DETECTOR_CONSTRUCTION_HPP
#define DETECTOR_CONSTRUCTION_HPP

#include "G4VUserDetectorConstruction.hh"

class G4VPhysicalVolume;

namespace G4GO::Detector {

class DetectorConstruction : public G4VUserDetectorConstruction {
public:
    DetectorConstruction();
    ~DetectorConstruction() override = default;

    auto Construct() -> G4VPhysicalVolume* override;
    auto ConstructSDandField() -> void override;
    auto ModuleID() const -> const auto& { return moduleID; }

private:
    G4bool fCheckOverlap;
    G4int moduleID{};
};

} // namespace G4GO::Detector

#endif
