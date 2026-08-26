#pragma once

#include "g4go/optical/Photon.hpp"
#include "g4go/optical/PhotonTransportOutput.hpp"
#include "g4go/optical/Scene.hpp"

#include <span>

namespace G4GO::Optical {

class PhotonTransport {
public:
    PhotonTransport() = default;
    virtual ~PhotonTransport() = default;

    PhotonTransport(const PhotonTransport&) = delete;
    auto operator=(const PhotonTransport&) -> PhotonTransport& = delete;

    virtual auto Propagate(const Scene& scene, std::span<const Photon> photons)
        -> PhotonTransportOutput = 0;
};

} // namespace G4GO::Optical
