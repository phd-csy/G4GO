#pragma once

#include "G4Types.hh"
#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class G4Track;
class G4VPhysicalVolume;
class G4CerenkovQuasiTrackInfo;
class G4GOQuasiScintillationTrackInfo;

namespace G4GO::Optical {

class OpticalBatchScheduler;

class Geant4EventAdapter final {
public:
    explicit Geant4EventAdapter(PhotonTransportConfig configuration,
                                std::shared_ptr<OpticalBatchScheduler> batchScheduler);
    ~Geant4EventAdapter();

    Geant4EventAdapter(const Geant4EventAdapter&) = delete;
    auto operator=(const Geant4EventAdapter&) -> Geant4EventAdapter& = delete;

    auto BeginRun() -> void;
    auto EndRun() -> void;
    auto BeginEvent(G4int eventID) -> void;
    auto EndEvent() -> PhotonTransportFuture;

    auto ObserveGenerated(const G4Track& track) -> void;
    auto Capture(const G4Track& track) -> void;
    auto CaptureOffloaded(const G4Track& track) -> void;

    auto SelectedBackend() const -> PhotonTransportBackend;
    auto Configuration() const -> const PhotonTransportConfig& { return fConfiguration; }
    auto EventStatistics() const -> const PhotonTransportStatistics& { return fEventStatistics; }
    auto RunPerformance() const -> const PhotonTransportPerformance& { return fRunPerformance; }
    auto RunStatistics() const -> const PhotonTransportStatistics&;

private:
    auto AppendOffloadedEmission(const G4Track& track, OpticalEmission emission, std::size_t photonCount,
                                 std::chrono::steady_clock::time_point captureStart, const char* operation,
                                 const char* volumeDescription) -> bool;
    auto CaptureCerenkov(const G4Track& track, const G4CerenkovQuasiTrackInfo& info) -> void;
    auto CaptureScintillation(const G4Track& track, const G4GOQuasiScintillationTrackInfo& info) -> void;
    auto LocateVolume(const G4Track& track) const -> const G4VPhysicalVolume*;
    auto VolumeIDFromTrack(const G4Track& track, const G4VPhysicalVolume* locatedVolume) const -> std::uint32_t;
    auto MaterialIDFromVolume(std::uint32_t volumeID) const -> std::uint32_t;
    auto AccumulateEventStatistics() -> void;

    struct VolumeLookupKey {
        std::uint32_t physicalVolumeID{};
        std::uint32_t copyNo{};
        std::uint32_t parentVolumeID{};

        auto operator==(const VolumeLookupKey&) const -> bool = default;
    };

    struct VolumeLookupKeyHash {
        auto operator()(const VolumeLookupKey& key) const -> std::size_t {
            auto hash{static_cast<std::size_t>(key.physicalVolumeID)};
            hash ^= static_cast<std::size_t>(key.copyNo) + static_cast<std::size_t>(0x9e3779b9U) + (hash << 6U) +
                    (hash >> 2U);
            hash ^= static_cast<std::size_t>(key.parentVolumeID) + static_cast<std::size_t>(0x9e3779b9U) +
                    (hash << 6U) + (hash >> 2U);
            return hash;
        }
    };

    PhotonTransportBackend fRequestedBackend;
    PhotonTransportConfig fConfiguration;
    std::vector<OpticalEmission> fEmissions;
    mutable std::unordered_map<VolumeLookupKey, std::uint32_t, VolumeLookupKeyHash> fVolumeIDCache;
    mutable const G4VPhysicalVolume* fCachedLocatedVolume;
    mutable std::uint32_t fCachedLocatedCopyNo;
    mutable std::uint32_t fCachedVolumeID;
    std::shared_ptr<OpticalBatchScheduler> fBatchScheduler;
    PhotonTransportStatistics fEventStatistics;
    PhotonTransportPerformance fEventPerformance;
    PhotonTransportStatistics fRunStatistics;
    PhotonTransportPerformance fRunPerformance;
    G4int fEventID;
    std::uint32_t fNextPhotonID;
    std::uint64_t fEventPhotonCount;
};

} // namespace G4GO::Optical
