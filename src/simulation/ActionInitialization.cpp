#include "g4go/simulation/ActionInitialization.hpp"

#include "g4go/optical/geant4/EventBridge.hpp"
#include "g4go/optical/geant4/OpticalBatchService.hpp"
#include "g4go/optical/geant4/StackingAction.hpp"
#include "g4go/simulation/EventAction.hpp"
#include "g4go/simulation/EventOutput.hpp"
#include "g4go/simulation/PrimaryGeneratorAction.hpp"
#include "g4go/simulation/RunAction.hpp"

#include <memory>
#include <utility>

namespace G4GO::Simulation {

ActionInitialization::ActionInitialization(
    G4GO::Optical::TransportConfig config) :
    fConfig{config},
    fBatchService{
        std::make_shared<G4GO::Optical::OpticalBatchService>(std::move(config))} {}

auto ActionInitialization::BuildForMaster() const -> void {
    SetUserAction(new RunAction(fBatchService, {}, {}, true));
}

auto ActionInitialization::Build() const -> void {
    const auto output{std::make_shared<EventOutputQueue>()};
    const auto bridge{
        std::make_shared<G4GO::Optical::OpticalEventBridge>(
            fConfig, fBatchService)};

    SetUserAction(new PrimaryGeneratorAction());
    SetUserAction(new RunAction(fBatchService, bridge, output));
    SetUserAction(new EventAction(bridge, output));
    SetUserAction(new G4GO::Optical::OpticalStackingAction(bridge));
}

} // namespace G4GO::Simulation
