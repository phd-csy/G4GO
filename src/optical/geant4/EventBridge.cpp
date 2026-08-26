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
#include "SceneExporter.hpp"
#include "g4go/optical/Transport.hpp"

#ifdef G4GO_ENABLE_OPTIX
#    include "g4go/optical/optix/Transport.hpp"
#endif

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace G4GO::Optical {

OpticalEventBridge::OpticalEventBridge(TransportConfig config) :
    fRequestedBackend{config.fBackend},
    fConfig{std::move(config)} {
    fPhotonData.reserve(std::min<std::size_t>(fConfig.fMaxPhotonCount, 1024));
}

OpticalEventBridge::~OpticalEventBridge() = default;

auto OpticalEventBridge::BeginRun() -> void {
    fRunStats = {};
    fNextPhotonID = 0;
    fTransport.reset();
    fScene = {};
    fConfig.fBackend = fRequestedBackend;

    if (fConfig.fBackend == Backend::Auto) {
#ifdef G4GO_ENABLE_OPTIX
        fConfig.fBackend = Backend::Optix;
#else
        fConfig.fBackend = Backend::Geant4;
#endif
    }

    if (fConfig.fBackend != Backend::Optix) {
        return;
    }

    const auto* world{
        G4TransportationManager::GetTransportationManager()
            ->GetNavigatorForTracking()
            ->GetWorldVolume()};
    try {
        fScene = Geant4SceneExporter{}.Export(world);
    } catch (const std::exception& exception) {
        G4ExceptionDescription description{};
        description << "Optical scene export failed: " << exception.what();
        G4Exception("OpticalEventBridge::BeginRun", "G4GOOpticalBackend",
                    FatalException, description);
        return;
    }

#ifdef G4GO_ENABLE_OPTIX
    try {
        if (fConfig.fBackend == Backend::Optix) {
            fTransport = std::make_unique<OptixOpticalTransport>(fConfig);
        }
    } catch (const std::exception& exception) {
        if (fRequestedBackend == Backend::Auto) {
            G4cout << "[g4go] OptiX initialization failed: "
                   << exception.what()
                   << "; falling back to Geant4 backend" << G4endl;
            fConfig.fBackend = Backend::Geant4;
            return;
        }

        G4ExceptionDescription description{};
        description << "OptiX backend initialization failed: "
                    << exception.what();
        G4Exception("OpticalEventBridge::BeginRun", "G4GOOpticalBackend",
                    FatalException, description);
    }
#else
    G4ExceptionDescription description{};
    description << "OptiX backend requested, but this build does not contain "
                   "the OptiX backend";
    G4Exception("OpticalEventBridge::BeginRun", "G4GOOpticalBackend",
                FatalException, description);
#endif
}

auto OpticalEventBridge::EndRun() -> void {
    fTransport.reset();
    fScene = {};
}

auto OpticalEventBridge::BeginEvent(G4int eventID) -> void {
    fEventID = eventID;
    fPhotonData.clear();
    fEventHits.clear();
    fEventStats = {};
}

auto OpticalEventBridge::EndEvent() -> void {
    if (fTransport != nullptr) {
        auto result{fTransport->Transport(fScene, fPhotonData)};
        fEventHits = std::move(result.fHitData);
        fEventStats.fDetectedCount = result.fStats.fDetectedCount;
        fEventStats.fAbsorbedCount = result.fStats.fAbsorbedCount;
        fEventStats.fEscapedCount = result.fStats.fEscapedCount;
        fEventStats.fTruncatedCount = result.fStats.fTruncatedCount;
        fEventStats.fMaxBounceCount = result.fStats.fMaxBounceCount;
        fEventStats.fInvalidStateCount = result.fStats.fInvalidStateCount;
        fEventStats.fZeroStepCount = result.fStats.fZeroStepCount;
        fEventStats.fTransportTimeMs = result.fStats.fTransportTimeMs;
    }
    AddEventStatsToRun();
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
