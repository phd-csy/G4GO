#pragma once

#include "G4UserStackingAction.hh"

#include <memory>

class G4Track;

namespace G4GO::Optical {

class Geant4EventAdapter;

} // namespace G4GO::Optical

namespace G4GO::Simulation {

class StackingAction final : public G4UserStackingAction {
public:
    explicit StackingAction(std::shared_ptr<G4GO::Optical::Geant4EventAdapter> adapter);
    ~StackingAction() override = default;

    auto ClassifyNewTrack(const G4Track* track) -> G4ClassificationOfNewTrack override;

private:
    std::shared_ptr<G4GO::Optical::Geant4EventAdapter> fAdapter;
};

} // namespace G4GO::Simulation
