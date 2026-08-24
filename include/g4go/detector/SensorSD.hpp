#ifndef SENSOR_SD_HPP
#define SENSOR_SD_HPP

#include "G4VSensitiveDetector.hh"
#include "g4go/detector/SensorHit.hpp"

namespace G4GO::Detector {

class SensorSD : public G4VSensitiveDetector {
public:
    SensorSD(const G4String&, const G4String&);
    ~SensorSD() override = default;

    auto Initialize(G4HCofThisEvent*) -> void override;
    auto ProcessHits(G4Step*, G4TouchableHistory*) -> G4bool override;
    auto EndOfEvent(G4HCofThisEvent*) -> void override;

private:
    SensorHC* hc{nullptr};
    G4int hcID{-1};
};

} // namespace G4GO::Detector

#endif
