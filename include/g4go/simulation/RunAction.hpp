#pragma once

#include "G4UserRunAction.hh"

#include <memory>

class G4Run;

namespace G4GO::Optical {
class OpticalEventBridge;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class RunAction : public G4UserRunAction {
public:
    explicit RunAction(
        std::shared_ptr<G4GO::Optical::OpticalEventBridge> bridge = {});
    ~RunAction() override = default;

    auto BeginOfRunAction(const G4Run* run) -> void override;
    auto EndOfRunAction(const G4Run* run) -> void override;

private:
    std::shared_ptr<G4GO::Optical::OpticalEventBridge> fBridge{};
};

} // namespace G4GO::Simulation
