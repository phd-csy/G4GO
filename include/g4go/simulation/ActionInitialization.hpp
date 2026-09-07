#pragma once

#include "G4VUserActionInitialization.hh"
#include "g4go/optical/PhotonTransportConfig.hpp"

#include <memory>

namespace G4GO::Optical {
class OpticalBatchScheduler;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class ActionInitialization : public G4VUserActionInitialization {
public:
    explicit ActionInitialization(G4GO::Optical::PhotonTransportConfig configuration);
    ~ActionInitialization() override = default;

    auto BuildForMaster() const -> void override;
    auto Build() const -> void override;

private:
    G4GO::Optical::PhotonTransportConfig fConfiguration;
    std::shared_ptr<G4GO::Optical::OpticalBatchScheduler> fBatchScheduler;
};

} // namespace G4GO::Simulation
