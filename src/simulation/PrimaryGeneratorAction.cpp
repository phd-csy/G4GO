#include "g4go/simulation/PrimaryGeneratorAction.hpp"

#include "CLHEP/Random/MTwistEngine.h"
#include "G4Event.hh"
#include "G4GeneralParticleSource.hh"
#include "Randomize.hh"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace G4GO::Simulation {

PrimaryGeneratorAction::PrimaryGeneratorAction(std::uint64_t seed) :
    G4VUserPrimaryGeneratorAction{},
    fParticleGun{std::make_unique<G4GeneralParticleSource>()},
    fSeed{seed} {}

auto PrimaryGeneratorAction::GeneratePrimaries(G4Event* anEvent) -> void {
    const auto eventSeed{fSeed + static_cast<std::uint64_t>(anEvent->GetEventID())};
    // MTwistEngine::setSeed replaces the state words but keeps the current draw index.
    // Restore the initial position so the event cannot inherit a previous event's draws.
    static const auto initialState{CLHEP::MTwistEngine{0L}.put()};
    if (not G4Random::getTheEngine()->get(initialState)) {
        throw std::runtime_error("Could not restore the MTwistEngine state for event " +
                                 std::to_string(anEvent->GetEventID()));
    }
    G4Random::setTheSeed(static_cast<long>(eventSeed));
    fParticleGun->GeneratePrimaryVertex(anEvent);
}

} // namespace G4GO::Simulation
