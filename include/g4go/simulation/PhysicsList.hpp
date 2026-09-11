#pragma once

#include "FTFP_BERT.hh"

namespace G4GO::Simulation {

// FTFP_BERT's Geant4-provided implementation uses multiple concrete base classes.
class PhysicsList final : public FTFP_BERT { // NOLINT(misc-multiple-inheritance)
public:
    explicit PhysicsList(G4bool useOpticalOffload);

    auto ConstructParticle() -> void override;
    auto ConstructProcess() -> void override;

private:
    G4bool fUseOpticalOffload{};
};

} // namespace G4GO::Simulation
