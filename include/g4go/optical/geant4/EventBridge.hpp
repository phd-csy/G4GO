#pragma once

#include "G4Types.hh"
#include "g4go/optical/Scene.hpp"
#include "g4go/optical/Types.hpp"

#include <memory>
#include <span>
#include <vector>

class G4Track;
class G4VPhysicalVolume;

namespace G4GO::Optical {

class OpticalTransport;

class OpticalEventBridge final {
public:
    explicit OpticalEventBridge(TransportConfig config);
    ~OpticalEventBridge();

    OpticalEventBridge(const OpticalEventBridge&) = delete;
    auto operator=(const OpticalEventBridge&) -> OpticalEventBridge& = delete;

    auto BeginRun() -> void;
    auto EndRun() -> void;
    auto BeginEvent(G4int eventID) -> void;
    auto EndEvent() -> void;

    auto ObserveGenerated() -> void;
    auto Capture(const G4Track& track) -> void;

    auto BackendType() const -> Backend { return fConfig.fBackend; }
    auto Config() const -> const TransportConfig& { return fConfig; }
    auto PhotonData() const -> std::span<const Photon> { return fPhotonData; }
    auto EventHits() const -> std::span<const PhotonHit> { return fEventHits; }
    auto EventStats() const -> const TransportStats& { return fEventStats; }
    auto RunStats() const -> const TransportStats& { return fRunStats; }

private:
    auto PhotonSourceFrom(const G4Track& track) const -> PhotonSource;
    auto LocateVolume(const G4Track& track) const -> const G4VPhysicalVolume*;
    auto VolumeIDFromTrack(const G4Track& track,
                           const G4VPhysicalVolume* locatedVolume) const
        -> std::uint32_t;
    auto AddEventStatsToRun() -> void;

    Backend fRequestedBackend{Backend::Auto};
    TransportConfig fConfig{};
    std::vector<Photon> fPhotonData{};
    std::vector<PhotonHit> fEventHits{};
    Scene fScene{};
    std::unique_ptr<OpticalTransport> fTransport{};
    TransportStats fEventStats{};
    TransportStats fRunStats{};
    G4int fEventID{-1};
    std::uint64_t fNextPhotonID{};
};

} // namespace G4GO::Optical
