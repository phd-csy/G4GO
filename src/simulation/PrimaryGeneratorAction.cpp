#include "g4go/simulation/PrimaryGeneratorAction.hpp"

#include "G4Event.hh"
#include "G4GeneralParticleSource.hh"
#include "Randomize.hh"

#include <cstdint>

namespace G4GO::Simulation {

PrimaryGeneratorAction::PrimaryGeneratorAction(std::uint64_t seed) :
    G4VUserPrimaryGeneratorAction{},
    fParticleGun{std::make_unique<G4GeneralParticleSource>()},
    fSeed{seed} {}

auto PrimaryGeneratorAction::GeneratePrimaries(G4Event* anEvent) -> void {
    const auto eventSeed{
        fSeed + static_cast<std::uint64_t>(anEvent->GetEventID())};
    G4Random::setTheSeed(static_cast<long>(eventSeed));
    fParticleGun->GeneratePrimaryVertex(anEvent);
}

} // namespace G4GO::Simulation
