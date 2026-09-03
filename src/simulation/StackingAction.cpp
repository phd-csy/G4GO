#include "g4go/simulation/StackingAction.hpp"

#include "G4OpticalPhoton.hh"
#include "G4QuasiOpticalPhoton.hh"
#include "G4Track.hh"
#include "G4VProcess.hh"
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
        if (track != nullptr &&
            track->GetDefinition() ==
                G4QuasiOpticalPhoton::QuasiOpticalPhotonDefinition() &&
            fAdapter->SelectedBackend() ==
                G4GO::Optical::PhotonTransportBackend::OptiX) {
            fAdapter->CaptureOffloaded(*track);
            return fKill;
        }
        return fUrgent;
    }

    if (fAdapter->SelectedBackend() ==
        G4GO::Optical::PhotonTransportBackend::Geant4) {
        fAdapter->ObserveGenerated(*track);
        return fUrgent;
    }

    if (const auto* creatorProcess{track->GetCreatorProcess()};
        creatorProcess != nullptr) {
        const auto& processName{creatorProcess->GetProcessName()};
        if (processName == "QuasiCerenkov" ||
            processName == "QuasiScintillation") {
            // Geant4's quasi scintillation process also materializes regular
            // optical secondaries while producing its offload metadata. The
            // quasi track is the authoritative GPU emission; discard this
            // duplicate host expansion before it reaches the adapter.
            return fKill;
        }
    }

    fAdapter->Capture(*track);
    return fKill;
}

} // namespace G4GO::Simulation
