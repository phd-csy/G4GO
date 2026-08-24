#pragma once

#include "G4VUserActionInitialization.hh"
#include "g4go/optical/Types.hpp"

namespace G4GO::Simulation {

class ActionInitialization : public G4VUserActionInitialization {
public:
    explicit ActionInitialization(G4GO::Optical::TransportConfig config);
    ~ActionInitialization() override = default;

    auto BuildForMaster() const -> void override;
    auto Build() const -> void override;

private:
    G4GO::Optical::TransportConfig fConfig{};
};

} // namespace G4GO::Simulation
