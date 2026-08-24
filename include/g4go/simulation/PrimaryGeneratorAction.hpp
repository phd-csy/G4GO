#ifndef PRIMARY_GENERATOR_ACTION_HPP
#define PRIMARY_GENERATOR_ACTION_HPP

#include "G4GeneralParticleSource.hh"
#include "G4VUserPrimaryGeneratorAction.hh"

#include <memory>

class G4Event;

namespace G4GO::Simulation {

class PrimaryGeneratorAction : public G4VUserPrimaryGeneratorAction {
public:
    PrimaryGeneratorAction();
    ~PrimaryGeneratorAction() override = default;

    auto GeneratePrimaries(G4Event* anEvent) -> void override;

private:
    std::unique_ptr<G4GeneralParticleSource> fParticleGun{};
};

} // namespace G4GO::Simulation

#endif
