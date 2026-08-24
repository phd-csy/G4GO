#pragma once

#include "g4go/optical/Transport.hpp"

namespace G4GO::Optical {

class CpuOpticalTransport final : public OpticalTransport {
public:
    explicit CpuOpticalTransport(TransportConfig config);
    ~CpuOpticalTransport() override = default;

    auto Transport(const Scene& scene,
                   std::span<const Photon> photonData)
        -> TransportResult override;

private:
    TransportConfig fConfig{};
};

} // namespace G4GO::Optical
