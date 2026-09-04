#pragma once

#include "G4VUserDetectorConstruction.hh"

class G4VPhysicalVolume;

namespace G4GO::Detector {

class DetectorConstruction : public G4VUserDetectorConstruction {
public:
    DetectorConstruction();
    ~DetectorConstruction() override = default;

    auto Construct() -> G4VPhysicalVolume* override;
    auto ConstructSDandField() -> void override;
    auto ModuleID() const -> const auto& { return fModuleID; }

private:
    G4bool fCheckOverlap;
    G4int fModuleID;
};

} // namespace G4GO::Detector
