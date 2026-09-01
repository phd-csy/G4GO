#include "g4go/optical/geant4/Geant4EventAdapter.hpp"

#include "G4Exception.hh"
#include "G4Navigator.hh"
#include "G4SystemOfUnits.hh"
#include "G4TouchableHistory.hh"
#include "G4Track.hh"
#include "G4TransportationManager.hh"
#include "G4VPhysicalVolume.hh"
#include "G4VProcess.hh"
#include "g4go/optical/geant4/Geant4BatchScheduler.hpp"

#include <algorithm>
#include <utility>

namespace G4GO::Optical {

Geant4EventAdapter::Geant4EventAdapter(
    PhotonTransportConfig configuration,
    std::shared_ptr<Geant4BatchScheduler> batchScheduler) :
    fRequestedBackend{configuration.fBackend},
    fConfiguration{std::move(configuration)},
    fBatchScheduler{std::move(batchScheduler)} {
    fPhotons.reserve(
        std::min<std::size_t>(
            fConfiguration.fMaxPhotonsPerEvent, 1024));
}

Geant4EventAdapter::~Geant4EventAdapter() = default;

auto Geant4EventAdapter::BeginRun() -> void {
    fRunStatistics = {};
    fNextPhotonID = 0;
    fConfiguration.fBackend = fRequestedBackend;
    if (fBatchScheduler) {
        fBatchScheduler->BeginRun();
        fConfiguration.fBackend = fBatchScheduler->SelectedBackend();
        return;
    }

    if (fConfiguration.fBackend == PhotonTransportBackend::Auto) {
#ifdef G4GO_ENABLE_OPTIX
        fConfiguration.fBackend = PhotonTransportBackend::OptiX;
#else
        fConfiguration.fBackend = PhotonTransportBackend::Geant4;
#endif
    }
}

auto Geant4EventAdapter::EndRun() -> void {
    fEventDetections.clear();
}

auto Geant4EventAdapter::BeginEvent(G4int eventID) -> void {
    fEventID = eventID;
    fNextPhotonID = 0;
    fPhotons.clear();
    fEventDetections.clear();
    fEventStatistics = {};
}

auto Geant4EventAdapter::EndEvent() -> PhotonTransportFuture {
    if (fConfiguration.fBackend == PhotonTransportBackend::OptiX &&
        fBatchScheduler) {
        return fBatchScheduler->Schedule(
            fEventID, std::move(fPhotons), fEventStatistics);
    }
    AccumulateEventStatistics();
    return {};
}

auto Geant4EventAdapter::SelectedBackend() const
    -> PhotonTransportBackend {
    return fConfiguration.fBackend;
}

auto Geant4EventAdapter::RunStatistics() const
    -> const PhotonTransportStatistics& {
    if (fBatchScheduler &&
        fConfiguration.fBackend == PhotonTransportBackend::OptiX) {
        return fBatchScheduler->RunStatistics();
    }
    return fRunStatistics;
}

auto Geant4EventAdapter::ObserveGenerated() -> void {
    ++fEventStatistics.fGeneratedCount;
}

auto Geant4EventAdapter::Capture(const G4Track& track) -> void {
    if (fPhotons.size() >= fConfiguration.fMaxPhotonsPerEvent) {
        G4ExceptionDescription description{};
        description << "Optical photon limit reached in event " << fEventID
                    << ": limit=" << fConfiguration.fMaxPhotonsPerEvent;
        G4Exception("Geant4EventAdapter::Capture", "G4GOPhotonLimit",
                    FatalException, description);
        return;
    }

    const auto* volume{LocateVolume(track)};
    if (volume == nullptr) {
        G4ExceptionDescription description{};
        description << "Unable to locate the creation volume for optical photon "
                    << track.GetTrackID() << " in event " << fEventID;
        G4Exception("Geant4EventAdapter::Capture", "G4GOVolumeLookup",
                    FatalException, description);
        return;
    }

    ++fEventStatistics.fGeneratedCount;

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
    photon.fVolumeID = VolumeIDFromTrack(track, volume);
    photon.fSource = PhotonSourceFrom(track);

    fPhotons.push_back(photon);
    ++fEventStatistics.fCapturedCount;
}

auto Geant4EventAdapter::PhotonSourceFrom(const G4Track& track) const
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

auto Geant4EventAdapter::LocateVolume(const G4Track& track) const
    -> const G4VPhysicalVolume* {
    auto* navigator{
        G4TransportationManager::GetTransportationManager()
            ->GetNavigatorForTracking()};
    auto position{track.GetPosition()};
    auto direction{track.GetMomentumDirection()};
    return navigator->LocateGlobalPointAndSetup(position, &direction, false);
}

auto Geant4EventAdapter::VolumeIDFromTrack(
    const G4Track& track, const G4VPhysicalVolume* locatedVolume) const
    -> std::uint32_t {
    const auto locatedID{static_cast<std::uint32_t>(
        std::max(locatedVolume->GetInstanceID(), 0))};
    if (!fBatchScheduler ||
        fBatchScheduler->ExportedScene().Volumes().empty()) {
        return locatedID;
    }

    const auto& scene{fBatchScheduler->ExportedScene()};

    const auto* touchable{track.GetTouchable()};
    if (touchable == nullptr) {
        return locatedID;
    }

    auto parentVolumeID{InvalidID};
    for (auto depth{touchable->GetHistoryDepth()}; depth >= 0; --depth) {
        const auto* physicalVolume{touchable->GetVolume(depth)};
        if (physicalVolume == nullptr) {
            return locatedID;
        }
        const auto copyNo{static_cast<std::uint32_t>(
            std::max(touchable->GetCopyNumber(depth), 0))};
        const auto physicalVolumeID{static_cast<std::uint32_t>(
            std::max(physicalVolume->GetInstanceID(), 0))};
        const auto* sceneVolume{scene.FindVolume(
            physicalVolumeID, copyNo, parentVolumeID)};
        if (sceneVolume == nullptr) {
            return locatedID;
        }
        parentVolumeID = sceneVolume->fVolumeID;
    }
    return parentVolumeID == InvalidID ? locatedID : parentVolumeID;
}

auto Geant4EventAdapter::AccumulateEventStatistics() -> void {
    fRunStatistics.fGeneratedCount += fEventStatistics.fGeneratedCount;
    fRunStatistics.fCapturedCount += fEventStatistics.fCapturedCount;
    fRunStatistics.fDetectedCount += fEventStatistics.fDetectedCount;
    fRunStatistics.fAbsorbedCount += fEventStatistics.fAbsorbedCount;
    fRunStatistics.fEscapedCount += fEventStatistics.fEscapedCount;
    fRunStatistics.fTruncatedCount += fEventStatistics.fTruncatedCount;
    fRunStatistics.fMaxBounceCount =
        std::max(fRunStatistics.fMaxBounceCount,
                 fEventStatistics.fMaxBounceCount);
    fRunStatistics.fInvalidStateCount +=
        fEventStatistics.fInvalidStateCount;
    fRunStatistics.fZeroStepCount += fEventStatistics.fZeroStepCount;
    fRunStatistics.fTransportTimeMs +=
        fEventStatistics.fTransportTimeMs;
}

} // namespace G4GO::Optical
