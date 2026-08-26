#include "g4go/optical/geant4/EventBridge.hpp"

#include "G4Exception.hh"
#include "G4Navigator.hh"
#include "G4OpticalPhoton.hh"
#include "G4ParticleDefinition.hh"
#include "G4SystemOfUnits.hh"
#include "G4Track.hh"
#include "G4TransportationManager.hh"
#include "G4VPhysicalVolume.hh"
#include "G4VProcess.hh"
#include "G4ios.hh"
#include "g4go/optical/geant4/OpticalBatchService.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace G4GO::Optical {

OpticalEventBridge::OpticalEventBridge(
    TransportConfig config,
    std::shared_ptr<OpticalBatchService> batchService) :
    fRequestedBackend{config.fBackend},
    fConfig{std::move(config)},
    fBatchService{std::move(batchService)} {
    fPhotonData.reserve(std::min<std::size_t>(fConfig.fMaxPhotonCount, 1024));
}

OpticalEventBridge::~OpticalEventBridge() = default;

auto OpticalEventBridge::BeginRun() -> void {
    fRunStats = {};
    fNextPhotonID = 0;
    fConfig.fBackend = fRequestedBackend;
    if (fBatchService) {
        fBatchService->BeginRun();
        fConfig.fBackend = fBatchService->BackendType();
        return;
    }

    if (fConfig.fBackend == Backend::Auto) {
#ifdef G4GO_ENABLE_OPTIX
        fConfig.fBackend = Backend::Optix;
#else
        fConfig.fBackend = Backend::Geant4;
#endif
    }
}

auto OpticalEventBridge::EndRun() -> void {
    fEventHits.clear();
}

auto OpticalEventBridge::BeginEvent(G4int eventID) -> void {
    fEventID = eventID;
    fPhotonData.clear();
    fEventHits.clear();
    fEventStats = {};
}

auto OpticalEventBridge::EndEvent() -> EventTransportFuture {
    if (fConfig.fBackend == Backend::Optix && fBatchService) {
        return fBatchService->Submit(fEventID, std::move(fPhotonData),
                                     fEventStats);
    }
    AddEventStatsToRun();
    return {};
}

auto OpticalEventBridge::BackendType() const -> Backend {
    return fConfig.fBackend;
}

auto OpticalEventBridge::RunStats() const -> const TransportStats& {
    if (fBatchService && fConfig.fBackend == Backend::Optix) {
        return fBatchService->RunStats();
    }
    return fRunStats;
}

auto OpticalEventBridge::ObserveGenerated() -> void {
    ++fEventStats.fGeneratedCount;
}

auto OpticalEventBridge::Capture(const G4Track& track) -> void {
    if (fPhotonData.size() >= fConfig.fMaxPhotonCount) {
        G4ExceptionDescription description{};
        description << "Optical photon limit reached in event " << fEventID
                    << ": limit=" << fConfig.fMaxPhotonCount;
        G4Exception("OpticalEventBridge::Capture", "G4GOPhotonLimit",
                    FatalException, description);
        return;
    }

    const auto* volume{LocateVolume(track)};
    if (volume == nullptr) {
        G4ExceptionDescription description{};
        description << "Unable to locate the creation volume for optical photon "
                    << track.GetTrackID() << " in event " << fEventID;
        G4Exception("OpticalEventBridge::Capture", "G4GOVolumeLookup",
                    FatalException, description);
        return;
    }

    ++fEventStats.fGeneratedCount;

    const auto position{track.GetPosition()};
    const auto direction{track.GetMomentumDirection()};
    const auto polarization{track.GetPolarization()};

    Photon photon{};
    photon.fPositionMm = {
        static_cast<float>(position.x() / mm),
        static_cast<float>(position.y() / mm),
        static_cast<float>(position.z() / mm),
    };
    photon.fTimeNs = static_cast<float>(track.GetGlobalTime() / ns);
    photon.fDirection = {
        static_cast<float>(direction.x()),
        static_cast<float>(direction.y()),
        static_cast<float>(direction.z()),
    };
    photon.fEnergyEv = static_cast<float>(track.GetKineticEnergy() / eV);
    photon.fPolarization = {
        static_cast<float>(polarization.x()),
        static_cast<float>(polarization.y()),
        static_cast<float>(polarization.z()),
    };
    photon.fEventID = static_cast<std::uint32_t>(fEventID);
    photon.fPhotonID = fNextPhotonID++;
    photon.fVolumeID = static_cast<std::uint32_t>(volume->GetInstanceID());
    photon.fSource = PhotonSourceFrom(track);

    fPhotonData.push_back(photon);
    ++fEventStats.fCapturedCount;
}

auto OpticalEventBridge::PhotonSourceFrom(const G4Track& track) const
    -> PhotonSource {
    const auto* creatorProcess{track.GetCreatorProcess()};
    if (creatorProcess == nullptr) {
        return PhotonSource::Unknown;
    }

    const auto& processName{creatorProcess->GetProcessName()};
    if (processName == "Scintillation") {
        return PhotonSource::Scintillation;
    }
    if (processName == "Cerenkov" || processName == "Cherenkov") {
        return PhotonSource::Cerenkov;
    }
    return PhotonSource::Unknown;
}

auto OpticalEventBridge::LocateVolume(const G4Track& track) const
    -> const G4VPhysicalVolume* {
    auto* navigator{
        G4TransportationManager::GetTransportationManager()
            ->GetNavigatorForTracking()};
    auto position{track.GetPosition()};
    auto direction{track.GetMomentumDirection()};
    return navigator->LocateGlobalPointAndSetup(position, &direction, false);
}

auto OpticalEventBridge::AddEventStatsToRun() -> void {
    fRunStats.fGeneratedCount += fEventStats.fGeneratedCount;
    fRunStats.fCapturedCount += fEventStats.fCapturedCount;
    fRunStats.fDetectedCount += fEventStats.fDetectedCount;
    fRunStats.fAbsorbedCount += fEventStats.fAbsorbedCount;
    fRunStats.fEscapedCount += fEventStats.fEscapedCount;
    fRunStats.fTruncatedCount += fEventStats.fTruncatedCount;
    fRunStats.fMaxBounceCount =
        std::max(fRunStats.fMaxBounceCount, fEventStats.fMaxBounceCount);
    fRunStats.fInvalidStateCount += fEventStats.fInvalidStateCount;
    fRunStats.fZeroStepCount += fEventStats.fZeroStepCount;
    fRunStats.fTransportTimeMs += fEventStats.fTransportTimeMs;
}

} // namespace G4GO::Optical
