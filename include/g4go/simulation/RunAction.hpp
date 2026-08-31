#pragma once

#include "G4UserRunAction.hh"

#include <memory>

class G4Run;

namespace G4GO::Optical {
class Geant4EventAdapter;
class Geant4BatchScheduler;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class Analysis;

class RunAction : public G4UserRunAction {
public:
    explicit RunAction(
        std::shared_ptr<G4GO::Optical::Geant4BatchScheduler> batchScheduler =
            {},
        std::shared_ptr<G4GO::Optical::Geant4EventAdapter> adapter = {},
        std::shared_ptr<Analysis> analysis = {},
        bool isMaster = false);
    ~RunAction() override = default;

    auto BeginOfRunAction(const G4Run* run) -> void override;
    auto EndOfRunAction(const G4Run* run) -> void override;

private:
    std::shared_ptr<G4GO::Optical::Geant4EventAdapter> fAdapter{};
    std::shared_ptr<G4GO::Optical::Geant4BatchScheduler> fBatchScheduler{};
    std::shared_ptr<Analysis> fAnalysis{};
    bool fIsMaster{};
};

} // namespace G4GO::Simulation
