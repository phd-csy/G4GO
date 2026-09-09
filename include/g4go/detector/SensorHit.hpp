#pragma once

#include "G4THitsCollection.hh"
#include "G4VHit.hh"

namespace G4GO::Detector {

class SensorHit : public G4VHit {
public:
    SensorHit() :
        G4VHit{},
        fGlobalTime{},
        fCopyNo{-1} {}
    ~SensorHit() override = default;

    auto GlobalTime(G4double time) -> void { fGlobalTime = time; }
    auto GlobalTime() const -> G4double { return fGlobalTime; }

    auto CopyNo(G4int copyNo) -> void { fCopyNo = copyNo; }
    auto CopyNo() const -> G4int { return fCopyNo; }

private:
    G4double fGlobalTime;
    G4int fCopyNo;
};

using SensorHC = G4THitsCollection<SensorHit>;

} // namespace G4GO::Detector
