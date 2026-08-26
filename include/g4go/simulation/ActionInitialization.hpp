#pragma once

#include "G4VUserActionInitialization.hh"
#include "g4go/optical/Types.hpp"

#include <memory>

namespace G4GO::Optical {
class OpticalBatchService;
} // namespace G4GO::Optical

namespace G4GO::Simulation {

class ActionInitialization : public G4VUserActionInitialization {
public:
    explicit ActionInitialization(G4GO::Optical::TransportConfig config);
    ~ActionInitialization() override = default;

    auto BuildForMaster() const -> void override;
    auto Build() const -> void override;

private:
    G4GO::Optical::TransportConfig fConfig{};
    std::shared_ptr<G4GO::Optical::OpticalBatchService> fBatchService{};
};

} // namespace G4GO::Simulation
