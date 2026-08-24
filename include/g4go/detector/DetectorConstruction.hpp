#ifndef DETECTOR_CONSTRUCTION_HPP
#define DETECTOR_CONSTRUCTION_HPP

#include "G4VUserDetectorConstruction.hh"
#include "globals.hh"

class G4VPhysicalVolume;

namespace G4GO::Detector {

class DetectorConstruction : public G4VUserDetectorConstruction {
public:
    DetectorConstruction() = default;
    ~DetectorConstruction() override = default;

    auto Construct() -> G4VPhysicalVolume* override;
    auto ConstructSDandField() -> void override;
    auto GetCellNumber() const -> const auto& { return cellNumber; }

private:
    G4bool fCheckOverlaps{true};
    G4int cellNumber{};
};

} // namespace G4GO::Detector

#endif
