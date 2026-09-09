#pragma once

#include "G4VSensitiveDetector.hh"
#include "g4go/detector/SensorHit.hpp"

namespace G4GO::Detector {

inline constexpr char SensorSensitiveDetectorName[] = "SensorSD";

class SensorSD : public G4VSensitiveDetector {
public:
    SensorSD(const G4String&, const G4String&);
    ~SensorSD() override = default;

    auto Initialize(G4HCofThisEvent*) -> void override;
    auto ProcessHits(G4Step*, G4TouchableHistory*) -> G4bool override;

private:
    SensorHC* fHitsCollection;
    G4int fHitsCollectionID;
};

} // namespace G4GO::Detector
