#pragma once

#include "g4go/optical/Scene.hpp"

#include <span>

namespace G4GO::Optical {

class OpticalTransport {
public:
    OpticalTransport() = default;
    virtual ~OpticalTransport() = default;

    OpticalTransport(const OpticalTransport&) = delete;
    auto operator=(const OpticalTransport&) -> OpticalTransport& = delete;

    virtual auto Transport(const Scene& scene,
                           std::span<const Photon> photonData)
        -> TransportResult = 0;
};

} // namespace G4GO::Optical
