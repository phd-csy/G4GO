#include "g4go/simulation/PrimaryGeneratorAction.hpp"

#include "CLHEP/Random/MTwistEngine.h"
#include "G4Event.hh"
#include "G4GeneralParticleSource.hh"
#include "G4Run.hh"
#include "G4RunManager.hh"
#include "Randomize.hh"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>

namespace G4GO::Simulation {

PrimaryGeneratorAction::PrimaryGeneratorAction(std::uint64_t seed) :
    G4VUserPrimaryGeneratorAction{},
    fParticleGun{std::make_unique<G4GeneralParticleSource>()},
    fSeed{seed} {}

auto PrimaryGeneratorAction::GeneratePrimaries(G4Event* anEvent) -> void {
    const auto runID{G4RunManager::GetRunManager()->GetCurrentRun()->GetRunID()};
    std::seed_seq seeds{static_cast<std::uint32_t>(fSeed), static_cast<std::uint32_t>(fSeed >> 32U),
                        static_cast<std::uint32_t>(runID), static_cast<std::uint32_t>(anEvent->GetEventID())};
    // CLHEP serializes the engine ID, 624 state words and the next draw index, in that order.
    // Preserve the ID and start every event at the same index, independent of its worker's history.
    static thread_local auto state{CLHEP::MTwistEngine{0L}.put()};
    seeds.generate(state.begin() + 1, state.end() - 1);
    if (not G4Random::getTheEngine()->get(state)) {
        throw std::runtime_error("Could not restore the MTwistEngine state for event " +
                                 std::to_string(anEvent->GetEventID()));
    }
    fParticleGun->GeneratePrimaryVertex(anEvent);
}

} // namespace G4GO::Simulation
