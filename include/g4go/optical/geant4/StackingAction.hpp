#pragma once

#include "G4UserStackingAction.hh"

#include <memory>

class G4Track;

namespace G4GO::Optical {

class OpticalEventBridge;

class OpticalStackingAction final : public G4UserStackingAction {
public:
    explicit OpticalStackingAction(std::shared_ptr<OpticalEventBridge> bridge);
    ~OpticalStackingAction() override = default;

    auto ClassifyNewTrack(const G4Track* track)
        -> G4ClassificationOfNewTrack override;

private:
    std::shared_ptr<OpticalEventBridge> fBridge{};
};

} // namespace G4GO::Optical
