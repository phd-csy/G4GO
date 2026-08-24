#pragma once

#include "g4go/optical/Transport.hpp"

#include <memory>

namespace G4GO::Optical {

class OptixOpticalTransport final : public OpticalTransport {
public:
    explicit OptixOpticalTransport(TransportConfig config);
    ~OptixOpticalTransport() override;

    OptixOpticalTransport(const OptixOpticalTransport&) = delete;
    auto operator=(const OptixOpticalTransport&) -> OptixOpticalTransport& = delete;

    auto Transport(const Scene& scene, std::span<const Photon> photonData)
        -> TransportResult override;

private:
    class Impl;
    std::unique_ptr<Impl> fImpl{};
};

} // namespace G4GO::Optical
