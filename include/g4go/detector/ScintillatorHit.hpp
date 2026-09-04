#pragma once

#include "G4THitsCollection.hh"
#include "G4VHit.hh"

namespace G4GO::Detector {

class ScintillatorHit : public G4VHit {
public:
    ScintillatorHit() : G4VHit{}, energyDeposit{} {}
    ~ScintillatorHit() override = default;

    auto AddEnergyDeposit(G4double eDep) -> void { energyDeposit += eDep; }
    auto EnergyDeposit() const -> G4double { return energyDeposit; }

private:
    G4double energyDeposit;
};

using ScintillatorHC = G4THitsCollection<ScintillatorHit>;

} // namespace G4GO::Detector
