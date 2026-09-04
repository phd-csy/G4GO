#pragma once

#include "G4VUserActionInitialization.hh"
#include "g4go/optical/PhotonTransportConfig.hpp"

#include <memory>

namespace G4GO::Optical {
class G4GOBatchScheduler;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class ActionInitialization : public G4VUserActionInitialization {
public:
    explicit ActionInitialization(
        G4GO::Optical::PhotonTransportConfig configuration);
    ~ActionInitialization() override = default;

    auto BuildForMaster() const -> void override;
    auto Build() const -> void override;

private:
    G4GO::Optical::PhotonTransportConfig fConfiguration;
    std::shared_ptr<G4GO::Optical::G4GOBatchScheduler> fBatchScheduler;
};

} // namespace G4GO::Simulation
