#include "g4go/simulation/ActionInitialization.hpp"

#include "g4go/optical/geant4/Geant4EventAdapter.hpp"
#include "g4go/optical/geant4/Geant4BatchScheduler.hpp"
#include "g4go/simulation/EventAction.hpp"
#include "g4go/simulation/Analysis.hpp"
#include "g4go/simulation/PrimaryGeneratorAction.hpp"
#include "g4go/simulation/RunAction.hpp"
#include "g4go/simulation/StackingAction.hpp"

#include <memory>
#include <utility>

namespace G4GO::Simulation {

ActionInitialization::ActionInitialization(
    G4GO::Optical::PhotonTransportConfig configuration) :
    fConfiguration{configuration},
    fBatchScheduler{
        std::make_shared<G4GO::Optical::Geant4BatchScheduler>(
            std::move(configuration))} {}

auto ActionInitialization::BuildForMaster() const -> void {
    SetUserAction(new RunAction(fBatchScheduler, {}, {}, true));
}

auto ActionInitialization::Build() const -> void {
    const auto analysis{std::make_shared<Analysis>()};
    const auto adapter{
        std::make_shared<G4GO::Optical::Geant4EventAdapter>(
            fConfiguration, fBatchScheduler)};

    SetUserAction(new PrimaryGeneratorAction());
    SetUserAction(new RunAction(fBatchScheduler, adapter, analysis));
    SetUserAction(new EventAction(adapter, analysis));
    SetUserAction(new StackingAction(adapter));
}

} // namespace G4GO::Simulation
