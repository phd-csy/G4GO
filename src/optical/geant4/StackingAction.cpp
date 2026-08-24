#include "g4go/optical/geant4/StackingAction.hpp"

#include "G4OpticalPhoton.hh"
#include "G4Track.hh"
#include "g4go/optical/geant4/EventBridge.hpp"

#include <stdexcept>
#include <utility>

namespace G4GO::Optical {

OpticalStackingAction::OpticalStackingAction(
    std::shared_ptr<OpticalEventBridge> bridge) :
    fBridge{std::move(bridge)} {
    if (!fBridge) {
        throw std::invalid_argument("OpticalStackingAction requires a bridge");
    }
}

auto OpticalStackingAction::ClassifyNewTrack(const G4Track* track)
    -> G4ClassificationOfNewTrack {
    if (track == nullptr ||
        track->GetDefinition() != G4OpticalPhoton::OpticalPhotonDefinition()) {
        return fUrgent;
    }

    if (fBridge->BackendType() == Backend::Geant4) {
        fBridge->ObserveGenerated();
        return fUrgent;
    }

    fBridge->Capture(*track);
    return fKill;
}

} // namespace G4GO::Optical
