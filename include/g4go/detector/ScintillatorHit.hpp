#ifndef SCINTILLATOR_HIT_HPP
#define SCINTILLATOR_HIT_HPP

#include "G4THitsCollection.hh"
#include "G4VHit.hh"

namespace G4GO::Detector {

class ScintillatorHit : public G4VHit {
public:
    ~ScintillatorHit() override = default;

    auto AddEnergyDeposit(G4double eDep) -> void { energyDeposit += eDep; }
    auto GetEnergyDeposit() const -> G4double { return energyDeposit; }

private:
    G4double energyDeposit{0.};
};

using ScintillatorHC = G4THitsCollection<ScintillatorHit>;

} // namespace G4GO::Detector

#endif
