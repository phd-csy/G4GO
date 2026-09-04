#pragma once

#include "FTFP_BERT.hh"

namespace G4GO::Simulation {

class PhysicsList final : public FTFP_BERT {
public:
    explicit PhysicsList(G4bool useOpticalOffload);

    auto ConstructParticle() -> void override;
    auto ConstructProcess() -> void override;

private:
    G4bool fUseOpticalOffload{};
};

} // namespace G4GO::Simulation
