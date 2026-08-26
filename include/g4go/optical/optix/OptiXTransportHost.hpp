#pragma once

#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"

#include <memory>

namespace G4GO::Optical {

class OptiXTransportHost final : public PhotonTransport {
public:
    explicit OptiXTransportHost(PhotonTransportConfig configuration);
    ~OptiXTransportHost() override;

    OptiXTransportHost(const OptiXTransportHost&) = delete;
    auto operator=(const OptiXTransportHost&) -> OptiXTransportHost& = delete;

    auto Propagate(const Scene& scene, std::span<const Photon> photons)
        -> PhotonTransportOutput override;

private:
    class Impl;
    std::unique_ptr<Impl> fImpl{};
};

} // namespace G4GO::Optical
