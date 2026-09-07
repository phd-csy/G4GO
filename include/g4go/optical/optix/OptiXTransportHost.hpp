#pragma once

#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"

#include <memory>

namespace G4GO::Optical {

class OptiXTransportHost final {
public:
    explicit OptiXTransportHost(PhotonTransportConfig configuration);
    ~OptiXTransportHost();

    OptiXTransportHost(const OptiXTransportHost&) = delete;
    auto operator=(const OptiXTransportHost&) -> OptiXTransportHost& = delete;

    auto PrepareScene(const Scene& scene) -> void;

    auto EnqueueEmissions(const Scene& scene, const PhotonTransportBatch& batch) -> void;
    auto CompleteOldestBatch() -> PhotonTransportOutput;

private:
    class Impl;
    std::unique_ptr<Impl> fImpl;
};

} // namespace G4GO::Optical
