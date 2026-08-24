#ifndef SENSOR_HIT_HPP
#define SENSOR_HIT_HPP

#include "G4THitsCollection.hh"
#include "G4VHit.hh"

namespace G4GO::Detector {

class SensorHit : public G4VHit {
public:
    ~SensorHit() override = default;

    auto SetGlobalTime(G4double time) -> void { globalTime = time; }
    auto GetGlobalTime() const -> G4double { return globalTime; }

    auto SetCopyNo(G4int copyNumber) -> void { copyNo = copyNumber; }
    auto GetCopyNo() const -> G4int { return copyNo; }

private:
    G4double globalTime{0.};
    G4int copyNo{-1};
};

using SensorHC = G4THitsCollection<SensorHit>;

} // namespace G4GO::Detector

#endif
