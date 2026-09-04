#pragma once

#include "G4UserRunAction.hh"

#include <memory>

class G4Run;

namespace G4GO::Optical {
class G4GOEventAdapter;
class G4GOBatchScheduler;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class Analysis;

class RunAction : public G4UserRunAction {
public:
    explicit RunAction(
        std::shared_ptr<G4GO::Optical::G4GOBatchScheduler> batchScheduler =
            {},
        std::shared_ptr<G4GO::Optical::G4GOEventAdapter> adapter = {},
        std::shared_ptr<Analysis> analysis = {},
        bool isMaster = false);
    ~RunAction() override = default;

    auto BeginOfRunAction(const G4Run* run) -> void override;
    auto EndOfRunAction(const G4Run* run) -> void override;

private:
    std::shared_ptr<G4GO::Optical::G4GOEventAdapter> fAdapter;
    std::shared_ptr<G4GO::Optical::G4GOBatchScheduler> fBatchScheduler;
    std::shared_ptr<Analysis> fAnalysis;
    bool fIsMaster;
};

} // namespace G4GO::Simulation
