#pragma once

#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace G4GO::Optical {

class OptiXTransportHost final : public PhotonTransport {
public:
    explicit OptiXTransportHost(PhotonTransportConfig configuration);
    ~OptiXTransportHost() override;

    OptiXTransportHost(const OptiXTransportHost&) = delete;
    auto operator=(const OptiXTransportHost&) -> OptiXTransportHost& = delete;

    auto PrepareScene(const Scene& scene) -> void;

    auto EnqueueEmissions(const Scene& scene,
                          const PhotonTransportBatch& batch) -> void;
    auto CompleteOldestBatch() -> PhotonTransportOutput;
    auto PendingBatchCount() const -> std::size_t;

    auto PropagateEmissions(const Scene& scene,
                            const PhotonTransportBatch& batch)
        -> PhotonTransportOutput override;

private:
    class Impl;
    std::unique_ptr<Impl> fImpl;
};

} // namespace G4GO::Optical
