#pragma once

#include "G4THitsCollection.hh"
#include "G4VHit.hh"

namespace G4GO::Detector {

class SensorHit : public G4VHit {
public:
    SensorHit() : G4VHit{}, globalTime{}, copyNo{-1} {}
    ~SensorHit() override = default;

    auto GlobalTime(G4double time) -> void { globalTime = time; }
    auto GlobalTime() const -> G4double { return globalTime; }

    auto CopyNo(G4int copyNumber) -> void { copyNo = copyNumber; }
    auto CopyNo() const -> G4int { return copyNo; }

private:
    G4double globalTime;
    G4int copyNo;
};

using SensorHC = G4THitsCollection<SensorHit>;

} // namespace G4GO::Detector
