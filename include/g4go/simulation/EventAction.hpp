#pragma once

#include "G4UserEventAction.hh"

#include <memory>

class G4Event;

namespace G4GO::Optical {
class OpticalEventBridge;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class EventOutputQueue;

class EventAction : public G4UserEventAction {
public:
    explicit EventAction(
        std::shared_ptr<G4GO::Optical::OpticalEventBridge> bridge,
        std::shared_ptr<EventOutputQueue> output);
    ~EventAction() override = default;

    auto BeginOfEventAction(const G4Event*) -> void override;
    auto EndOfEventAction(const G4Event*) -> void override;

private:
    std::shared_ptr<G4GO::Optical::OpticalEventBridge> fBridge{};
    std::shared_ptr<EventOutputQueue> fOutput{};
};

} // namespace G4GO::Simulation
