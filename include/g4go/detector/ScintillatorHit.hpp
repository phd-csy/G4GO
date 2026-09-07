#pragma once

#include "G4THitsCollection.hh"
#include "G4VHit.hh"

namespace G4GO::Detector {

class ScintillatorHit : public G4VHit {
public:
    ScintillatorHit() :
        G4VHit{},
        fEnergyDeposit{} {}
    ~ScintillatorHit() override = default;

    auto AddEnergyDeposit(G4double eDep) -> void { fEnergyDeposit += eDep; }
    auto EnergyDeposit() const -> G4double { return fEnergyDeposit; }

private:
    G4double fEnergyDeposit;
};

using ScintillatorHC = G4THitsCollection<ScintillatorHit>;

} // namespace G4GO::Detector
