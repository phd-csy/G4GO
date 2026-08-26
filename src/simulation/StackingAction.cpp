#include "g4go/simulation/StackingAction.hpp"

#include "G4OpticalPhoton.hh"
#include "G4Track.hh"
#include "g4go/optical/geant4/Geant4EventAdapter.hpp"

#include <stdexcept>
#include <utility>

namespace G4GO::Simulation {

StackingAction::StackingAction(
    std::shared_ptr<G4GO::Optical::Geant4EventAdapter> adapter) :
    fAdapter{std::move(adapter)} {
    if (!fAdapter) {
        throw std::invalid_argument(
            "StackingAction requires an event adapter");
    }
}

auto StackingAction::ClassifyNewTrack(const G4Track* track)
    -> G4ClassificationOfNewTrack {
    if (track == nullptr ||
        track->GetDefinition() != G4OpticalPhoton::OpticalPhotonDefinition()) {
        return fUrgent;
    }

    if (fAdapter->SelectedBackend() ==
        G4GO::Optical::PhotonTransportBackend::Geant4) {
        fAdapter->ObserveGenerated();
        return fUrgent;
    }

    fAdapter->Capture(*track);
    return fKill;
}

} // namespace G4GO::Simulation
