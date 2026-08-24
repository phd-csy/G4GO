#ifndef SCINTILLATOR_SD_HPP
#define SCINTILLATOR_SD_HPP

#include "G4VSensitiveDetector.hh"
#include "g4go/detector/ScintillatorHit.hpp"

namespace G4GO::Detector {

class ScintillatorSD : public G4VSensitiveDetector {
public:
    ScintillatorSD(const G4String&, const G4String&);
    ~ScintillatorSD() override = default;

    auto Initialize(G4HCofThisEvent*) -> void override;
    auto ProcessHits(G4Step*, G4TouchableHistory*) -> G4bool override;

private:
    ScintillatorHC* hc{nullptr};
    G4int hcID{-1};
};

} // namespace G4GO::Detector

#endif
