#pragma once

#include "G4GeneralParticleSource.hh"
#include "G4VUserPrimaryGeneratorAction.hh"

#include <cstdint>
#include <memory>

class G4Event;

namespace G4GO::Simulation {

class PrimaryGeneratorAction : public G4VUserPrimaryGeneratorAction {
public:
    explicit PrimaryGeneratorAction(std::uint64_t seed = 0);
    ~PrimaryGeneratorAction() override = default;

    auto GeneratePrimaries(G4Event* anEvent) -> void override;

private:
    std::unique_ptr<G4GeneralParticleSource> fParticleGun;
    std::uint64_t fSeed;
};

} // namespace G4GO::Simulation
