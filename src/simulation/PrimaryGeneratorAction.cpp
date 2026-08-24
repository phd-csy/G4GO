#include "g4go/simulation/PrimaryGeneratorAction.hpp"

#include "G4Event.hh"
#include "G4GeneralParticleSource.hh"

namespace G4GO::Simulation {

PrimaryGeneratorAction::PrimaryGeneratorAction() :
    G4VUserPrimaryGeneratorAction{},
    fParticleGun{std::make_unique<G4GeneralParticleSource>()} {}

auto PrimaryGeneratorAction::GeneratePrimaries(G4Event* anEvent) -> void {
    fParticleGun->GeneratePrimaryVertex(anEvent);
}

} // namespace G4GO::Simulation
