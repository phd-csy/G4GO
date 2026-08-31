#pragma once

#include "G4Types.hh"
#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"

#include <memory>
#include <span>
#include <vector>

class G4Track;
class G4VPhysicalVolume;

namespace G4GO::Optical {

class Geant4BatchScheduler;

class Geant4EventAdapter final {
public:
    explicit Geant4EventAdapter(
        PhotonTransportConfig configuration,
        std::shared_ptr<Geant4BatchScheduler> batchScheduler = {});
    ~Geant4EventAdapter();

    Geant4EventAdapter(const Geant4EventAdapter&) = delete;
    auto operator=(const Geant4EventAdapter&) -> Geant4EventAdapter& = delete;

    auto BeginRun() -> void;
    auto EndRun() -> void;
    auto BeginEvent(G4int eventID) -> void;
    auto EndEvent() -> PhotonTransportFuture;

    auto ObserveGenerated() -> void;
    auto Capture(const G4Track& track) -> void;

    auto SelectedBackend() const -> PhotonTransportBackend;
    auto Configuration() const -> const PhotonTransportConfig& {
        return fConfiguration;
    }
    auto Photons() const -> std::span<const Photon> { return fPhotons; }
    auto EventDetections() const -> std::span<const PhotonDetection> {
        return fEventDetections;
    }
    auto EventStatistics() const -> const PhotonTransportStatistics& {
        return fEventStatistics;
    }
    auto RunStatistics() const -> const PhotonTransportStatistics&;

private:
    auto PhotonSourceFrom(const G4Track& track) const -> PhotonSource;
    auto LocateVolume(const G4Track& track) const -> const G4VPhysicalVolume*;
    auto VolumeIDFromTrack(const G4Track& track,
                           const G4VPhysicalVolume* locatedVolume) const
        -> std::uint32_t;
    auto AccumulateEventStatistics() -> void;

    PhotonTransportBackend fRequestedBackend{PhotonTransportBackend::Auto};
    PhotonTransportConfig fConfiguration{};
    std::vector<Photon> fPhotons{};
    std::vector<PhotonDetection> fEventDetections{};
    std::shared_ptr<Geant4BatchScheduler> fBatchScheduler{};
    PhotonTransportStatistics fEventStatistics{};
    PhotonTransportStatistics fRunStatistics{};
    G4int fEventID{-1};
    std::uint32_t fNextPhotonID{};
};

} // namespace G4GO::Optical
