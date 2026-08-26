#pragma once

#include "G4UserRunAction.hh"

#include <memory>

class G4Run;

namespace G4GO::Optical {
class OpticalEventBridge;
class OpticalBatchService;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class EventOutputQueue;

class RunAction : public G4UserRunAction {
public:
    explicit RunAction(
        std::shared_ptr<G4GO::Optical::OpticalBatchService> batchService = {},
        std::shared_ptr<G4GO::Optical::OpticalEventBridge> bridge = {},
        std::shared_ptr<EventOutputQueue> output = {},
        bool isMaster = false);
    ~RunAction() override = default;

    auto BeginOfRunAction(const G4Run* run) -> void override;
    auto EndOfRunAction(const G4Run* run) -> void override;

private:
    std::shared_ptr<G4GO::Optical::OpticalEventBridge> fBridge{};
    std::shared_ptr<G4GO::Optical::OpticalBatchService> fBatchService{};
    std::shared_ptr<EventOutputQueue> fOutput{};
    bool fIsMaster{};
};

} // namespace G4GO::Simulation
