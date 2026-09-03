#include "g4go/optical/geant4/Geant4EventAdapter.hpp"

#include "G4CerenkovQuasiTrackInfo.hh"
#include "G4Exception.hh"
#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4MaterialPropertyVector.hh"
#include "G4Navigator.hh"
#include "G4PhysicsModelCatalog.hh"
#include "G4ScintillationQuasiTrackInfo.hh"
#include "G4SystemOfUnits.hh"
#include "G4TouchableHistory.hh"
#include "G4Track.hh"
#include "G4TransportationManager.hh"
#include "G4VPhysicalVolume.hh"
#include "G4VProcess.hh"
#include "g4go/optical/geant4/Geant4BatchScheduler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace G4GO::Optical {

namespace {

auto MaterialFromIndex(std::size_t materialIndex) -> const G4Material* {
    const auto* materialTable{G4Material::GetMaterialTable()};
    if (materialTable == nullptr || materialIndex >= materialTable->size()) {
        return nullptr;
    }
    return materialTable->at(materialIndex);
}

auto VectorFrom(const G4ThreeVector& vector) -> std::array<float, 3> {
    return {static_cast<float>(vector.x()), static_cast<float>(vector.y()),
            static_cast<float>(vector.z())};
}

auto VectorFromMm(const G4ThreeVector& vector) -> std::array<float, 3> {
    return {static_cast<float>(vector.x() / mm),
            static_cast<float>(vector.y() / mm),
            static_cast<float>(vector.z() / mm)};
}

} // namespace

Geant4EventAdapter::Geant4EventAdapter(
    PhotonTransportConfig configuration,
    std::shared_ptr<Geant4BatchScheduler> batchScheduler) :
    fRequestedBackend{configuration.fBackend},
    fConfiguration{std::move(configuration)},
    fBatchScheduler{std::move(batchScheduler)} {
    if (!fBatchScheduler) {
        throw std::invalid_argument(
            "Geant4EventAdapter requires a batch scheduler");
    }
    fEmissions.reserve(
        std::min<std::size_t>(
            fConfiguration.fMaxPhotonsPerEvent, 1024));
}

Geant4EventAdapter::~Geant4EventAdapter() = default;

auto Geant4EventAdapter::BeginRun() -> void {
    fRunStatistics = {};
    fRunPerformance = {};
    fEventPerformance = {};
    fVolumeIDCache.clear();
    fCachedLocatedVolume = nullptr;
    fCachedLocatedCopyNo = 0;
    fCachedVolumeID = InvalidID;
    fNextPhotonID = 0;
    fNextEmissionID = 0;
    fEventPhotonCount = 0;
    fConfiguration.fBackend = fRequestedBackend;
    fBatchScheduler->BeginRun();
    fConfiguration.fBackend = fBatchScheduler->SelectedBackend();
}

auto Geant4EventAdapter::EndRun() -> void {
    fVolumeIDCache.clear();
}

auto Geant4EventAdapter::BeginEvent(G4int eventID) -> void {
    fEventID = eventID;
    fNextPhotonID = 0;
    fNextEmissionID = 0;
    fEventPhotonCount = 0;
    fEmissions.clear();
    fEventStatistics = {};
    fEventPerformance = {};
}

auto Geant4EventAdapter::EndEvent() -> PhotonTransportFuture {
    if (fConfiguration.fBackend == PhotonTransportBackend::OptiX) {
        OpticalSubmission submission{};
        submission.fEventID = static_cast<std::uint32_t>(fEventID);
        submission.fEmissions = std::move(fEmissions);
        submission.fPhotonCount = fEventPhotonCount;
        submission.fSourceStatistics = fEventStatistics;
        submission.fSourcePerformance = fEventPerformance;
        auto future{fBatchScheduler->Submit(std::move(submission))};
        fRunPerformance.Accumulate(fEventPerformance);
        return future;
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
    if (fConfiguration.fBackend == PhotonTransportBackend::OptiX) {
        return fBatchScheduler->RunStatistics();
    }
    return fRunStatistics;
}

auto Geant4EventAdapter::ObserveGenerated(const G4Track& track) -> void {
    ++fEventStatistics.fGeneratedCount;
    const auto* creatorProcess{track.GetCreatorProcess()};
    if (creatorProcess == nullptr) {
        return;
    }
    const auto& processName{creatorProcess->GetProcessName()};
    if (processName == "Cerenkov" || processName == "Cherenkov") {
        ++fEventPerformance.fCerenkovPhotonCount;
    } else if (processName == "Scintillation") {
        ++fEventPerformance.fScintillationPhotonCount;
    }
}

auto Geant4EventAdapter::Capture(const G4Track& track) -> void {
    const auto diagnostics{fConfiguration.fEnablePerformanceDiagnostics};
    const auto captureStart{
        diagnostics ? std::chrono::steady_clock::now() :
                      std::chrono::steady_clock::time_point{}};

    const auto position{track.GetPosition()};
    const auto direction{track.GetMomentumDirection()};
    const auto polarization{track.GetPolarization()};

    OpticalEmission emission{};
    emission.fPositionMm = {
        static_cast<float>(position.x() / mm),
        static_cast<float>(position.y() / mm),
        static_cast<float>(position.z() / mm),
    };
    emission.fTimeNs = static_cast<float>(track.GetGlobalTime() / ns);
    emission.fDirection = {
        static_cast<float>(direction.x()),
        static_cast<float>(direction.y()),
        static_cast<float>(direction.z()),
    };
    emission.fEnergyEv = static_cast<float>(track.GetKineticEnergy() / eV);
    emission.fPolarization = {
        static_cast<float>(polarization.x()),
        static_cast<float>(polarization.y()),
        static_cast<float>(polarization.z()),
    };
    emission.fType = OpticalEmissionType::Direct;
    AppendOffloadedEmission(track, std::move(emission), 1, captureStart,
                            "Geant4EventAdapter::Capture", "the creation");
}

auto Geant4EventAdapter::CaptureOffloaded(const G4Track& track) -> void {
    const auto cerenkovModelID{
        G4PhysicsModelCatalog::GetModelID("model_QuasiCerenkov")};
    if (const auto* info{G4CerenkovQuasiTrackInfo::Cast(
            track.GetAuxiliaryTrackInformation(cerenkovModelID))};
        info != nullptr) {
        CaptureCerenkov(track, *info);
        return;
    }

    const auto scintillationModelID{
        G4PhysicsModelCatalog::GetModelID("model_QuasiScintillation")};
    if (const auto* info{G4ScintillationQuasiTrackInfo::Cast(
            track.GetAuxiliaryTrackInformation(scintillationModelID))};
        info != nullptr) {
        CaptureScintillation(track, *info);
        return;
    }

    G4ExceptionDescription description{};
    description << "quasi-optical track " << track.GetTrackID()
                << " has no recognized offload metadata";
    G4Exception("Geant4EventAdapter::CaptureOffloaded",
                "G4GOUnknownOffloadTrack", FatalException, description);
}

auto Geant4EventAdapter::AppendOffloadedEmission(
    const G4Track& track,
    OpticalEmission emission,
    std::size_t photonCount,
    std::chrono::steady_clock::time_point captureStart,
    const char* operation,
    const char* volumeDescription) -> bool {
    const auto maxPhotons{
        static_cast<std::uint64_t>(fConfiguration.fMaxPhotonsPerEvent)};
    if (photonCount > maxPhotons || fEventPhotonCount > maxPhotons - photonCount) {
        G4ExceptionDescription description{};
        description << "Optical photon limit reached in event " << fEventID
                    << ": limit=" << fConfiguration.fMaxPhotonsPerEvent;
        G4Exception(operation, "G4GOPhotonLimit", FatalException, description);
        return false;
    }

    const auto diagnostics{fConfiguration.fEnablePerformanceDiagnostics};
    const auto locateStart{diagnostics ? std::chrono::steady_clock::now() :
                                         std::chrono::steady_clock::time_point{}};
    const auto* volume{LocateVolume(track)};
    if (diagnostics) {
        fEventPerformance.fCaptureLocateVolumeMs +=
            std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - locateStart}
                .count();
    }
    if (volume == nullptr) {
        G4ExceptionDescription description{};
        description << "Unable to locate " << volumeDescription
                    << " volume in event " << fEventID;
        G4Exception(operation, "G4GOVolumeLookup", FatalException, description);
        return false;
    }

    const auto mappingStart{diagnostics ? std::chrono::steady_clock::now() :
                                          std::chrono::steady_clock::time_point{}};
    const auto volumeID{VolumeIDFromTrack(track, volume)};
    if (diagnostics) {
        fEventPerformance.fCaptureVolumeMappingMs +=
            std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - mappingStart}
                .count();
    }
    if (!fBatchScheduler->ExportedScene().Volumes().empty() &&
        volumeID == InvalidID) {
        G4ExceptionDescription description{};
        description << "Unable to map " << volumeDescription
                    << " volume in event " << fEventID
                    << " to the exported scene";
        G4Exception(operation, "G4GOSceneVolumeLookup", FatalException,
                    description);
        return false;
    }

    emission.fEventID = static_cast<std::uint32_t>(fEventID);
    emission.fEmissionID = fNextEmissionID++;
    emission.fFirstPhotonID = fNextPhotonID;
    emission.fPhotonCount = static_cast<std::uint32_t>(photonCount);
    emission.fVolumeID = volumeID;
    emission.fMaterialID = MaterialIDFromVolume(volumeID);
    fNextPhotonID += static_cast<std::uint32_t>(photonCount);
    fEventPhotonCount += photonCount;
    fEmissions.emplace_back(std::move(emission));
    fEventStatistics.fGeneratedCount += photonCount;
    fEventStatistics.fCapturedCount += photonCount;
    if (diagnostics) {
        fEventPerformance.fCaptureTotalMs +=
            std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - captureStart}
                .count();
    }
    return true;
}

auto Geant4EventAdapter::CaptureCerenkov(
    const G4Track& track, const G4CerenkovQuasiTrackInfo& info) -> void {
    const auto diagnostics{fConfiguration.fEnablePerformanceDiagnostics};
    const auto captureStart{
        diagnostics ? std::chrono::steady_clock::now() :
                      std::chrono::steady_clock::time_point{}};
    const auto data{info.GetQuasiOpticalData()};
    const auto photonCount{data.num_photons > 0 ?
                               static_cast<std::size_t>(data.num_photons) :
                               std::size_t{}};
    if (photonCount == 0U) {
        return;
    }

    OpticalEmission emission{};
    emission.fPositionMm = VectorFromMm(track.GetPosition());
    emission.fTimeNs = static_cast<float>(track.GetGlobalTime() / ns);
    auto emissionDirection{data.delta_position};
    if (emissionDirection.mag2() > 0.0) {
        emissionDirection = emissionDirection.unit();
    } else {
        emissionDirection = track.GetMomentumDirection();
    }
    emission.fDirection = VectorFrom(emissionDirection);
    emission.fStepLengthMm = static_cast<float>(data.step_length / mm);
    emission.fStepDeltaMm = VectorFromMm(data.delta_position);
    emission.fPreVelocityMmPerNs =
        static_cast<float>(data.pre_velocity / (mm / ns));
    emission.fDeltaVelocityMmPerNs =
        static_cast<float>(data.delta_velocity / (mm / ns));
    emission.fCharge = static_cast<float>(data.charge);
    emission.fPreMeanPhotonCount =
        static_cast<float>(info.GetPreNumPhotons());
    emission.fPostMeanPhotonCount =
        static_cast<float>(info.GetPostNumPhotons());
    emission.fType = OpticalEmissionType::Cerenkov;
    if (AppendOffloadedEmission(
            track, std::move(emission), photonCount, captureStart,
            "Geant4EventAdapter::CaptureCerenkov", "Cerenkov offload")) {
        fEventPerformance.fCerenkovPhotonCount += photonCount;
    }
}

auto Geant4EventAdapter::CaptureScintillation(
    const G4Track& track, const G4ScintillationQuasiTrackInfo& info) -> void {
    const auto diagnostics{fConfiguration.fEnablePerformanceDiagnostics};
    const auto captureStart{
        diagnostics ? std::chrono::steady_clock::now() :
                      std::chrono::steady_clock::time_point{}};
    const auto data{info.GetQuasiOpticalData()};
    const auto photonCount{data.num_photons > 0 ?
                               static_cast<std::size_t>(data.num_photons) :
                               std::size_t{}};
    if (photonCount == 0U) {
        return;
    }

    const auto* material{MaterialFromIndex(data.mat_index)};
    const auto* properties{material == nullptr ?
                               nullptr :
                               material->GetMaterialPropertiesTable()};
    if (properties == nullptr) {
        G4ExceptionDescription description{};
        description << "scintillation offload metadata references material "
                    << data.mat_index << " without optical properties";
        G4Exception("Geant4EventAdapter::CaptureScintillation",
                    "G4GOScintillationProperties", FatalException,
                    description);
        return;
    }

    const std::array<const G4MaterialPropertyVector*, 3> components{
        properties->GetProperty(kSCINTILLATIONCOMPONENT1),
        properties->GetProperty(kSCINTILLATIONCOMPONENT2),
        properties->GetProperty(kSCINTILLATIONCOMPONENT3)};
    const std::array<G4int, 3> timeKeys{
        kSCINTILLATIONTIMECONSTANT1,
        kSCINTILLATIONTIMECONSTANT2,
        kSCINTILLATIONTIMECONSTANT3};
    const auto scintillationTime{info.GetScintTime()};
    const G4MaterialPropertyVector* spectrum{nullptr};
    for (auto component{std::size_t{}}; component < components.size();
         ++component) {
        if (components.at(component) == nullptr) {
            continue;
        }
        if (component == 0U ||
            (properties->ConstPropertyExists(timeKeys.at(component)) &&
             properties->GetConstProperty(timeKeys.at(component)) ==
                 scintillationTime)) {
            spectrum = components.at(component);
            if (component != 0U) {
                break;
            }
        }
    }
    if (spectrum == nullptr) {
        G4ExceptionDescription description{};
        description << "unable to select scintillation spectrum for material "
                    << data.mat_index;
        G4Exception("Geant4EventAdapter::CaptureScintillation",
                    "G4GOScintillationSpectrum", FatalException, description);
        return;
    }

    const auto spectrumID{static_cast<std::uint32_t>(
        std::distance(components.begin(),
                      std::find(components.begin(), components.end(),
                                spectrum)))};
    OpticalEmission emission{};
    emission.fPositionMm = VectorFromMm(track.GetPosition());
    emission.fTimeNs = static_cast<float>(track.GetGlobalTime() / ns);
    emission.fDirection = VectorFrom(track.GetMomentumDirection());
    emission.fStepLengthMm = static_cast<float>(data.step_length / mm);
    emission.fStepDeltaMm = VectorFromMm(data.delta_position);
    emission.fPreVelocityMmPerNs =
        static_cast<float>(data.pre_velocity / (mm / ns));
    emission.fDeltaVelocityMmPerNs =
        static_cast<float>(data.delta_velocity / (mm / ns));
    emission.fDecayTimeNs = static_cast<float>(info.GetScintTime() / ns);
    emission.fRiseTimeNs = static_cast<float>(info.GetRiseTime() / ns);
    emission.fCharge = static_cast<float>(data.charge);
    emission.fSpectrumID = spectrumID;
    emission.fType = OpticalEmissionType::Scintillation;
    if (AppendOffloadedEmission(
            track, std::move(emission), photonCount, captureStart,
            "Geant4EventAdapter::CaptureScintillation",
            "scintillation offload")) {
        fEventPerformance.fScintillationPhotonCount += photonCount;
    }
}

auto Geant4EventAdapter::LocateVolume(const G4Track& track) const
    -> const G4VPhysicalVolume* {
    if (const auto* volume{track.GetVolume()}; volume != nullptr) {
        return volume;
    }
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
    if (fBatchScheduler->ExportedScene().Volumes().empty()) {
        return locatedID;
    }

    const auto& scene{fBatchScheduler->ExportedScene()};
    const auto locatedCopyNo{static_cast<std::uint32_t>(
        std::max(locatedVolume->GetCopyNo(), 0))};
    if (locatedVolume == fCachedLocatedVolume &&
        locatedCopyNo == fCachedLocatedCopyNo &&
        fCachedVolumeID != InvalidID) {
        return fCachedVolumeID;
    }

    const auto findVolumeID{
        [&](std::uint32_t physicalVolumeID,
            std::uint32_t copyNo,
            std::uint32_t parentVolumeID) {
            const VolumeLookupKey key{
                physicalVolumeID, copyNo, parentVolumeID};
            if (const auto found{fVolumeIDCache.find(key)};
                found != fVolumeIDCache.end()) {
                return found->second;
            }
            const auto* sceneVolume{
                scene.FindVolume(physicalVolumeID, copyNo, parentVolumeID)};
            const auto volumeID{sceneVolume == nullptr ?
                                    InvalidID :
                                    sceneVolume->fVolumeID};
            fVolumeIDCache.emplace(key, volumeID);
            return volumeID;
        }};
    const auto findUniqueVolumeID{
        [&](std::uint32_t physicalVolumeID, std::uint32_t copyNo) {
            const VolumeLookupKey key{
                physicalVolumeID, copyNo, InvalidID};
            if (const auto found{fVolumeIDCache.find(key)};
                found != fVolumeIDCache.end()) {
                return found->second;
            }
            std::uint32_t volumeID{InvalidID};
            for (const auto& candidate : scene.Volumes()) {
                if (candidate.fPhysicalVolumeID != physicalVolumeID ||
                    candidate.fCopyNo != copyNo) {
                    continue;
                }
                if (volumeID != InvalidID) {
                    return InvalidID;
                }
                volumeID = candidate.fVolumeID;
            }
            if (volumeID != InvalidID) {
                fVolumeIDCache.emplace(key, volumeID);
            }
            return volumeID;
        }};

    if (const auto uniqueVolumeID{
            findUniqueVolumeID(locatedID, locatedCopyNo)};
        uniqueVolumeID != InvalidID) {
        fCachedLocatedVolume = locatedVolume;
        fCachedLocatedCopyNo = locatedCopyNo;
        fCachedVolumeID = uniqueVolumeID;
        return uniqueVolumeID;
    }

    const auto* touchable{track.GetTouchable()};
    if (touchable == nullptr) {
        return findVolumeID(locatedID, locatedCopyNo, InvalidID);
    }

    auto parentVolumeID{InvalidID};
    for (auto depth{touchable->GetHistoryDepth()}; depth >= 0; --depth) {
        const auto* physicalVolume{touchable->GetVolume(depth)};
        if (physicalVolume == nullptr) {
            return InvalidID;
        }
        const auto copyNo{static_cast<std::uint32_t>(
            std::max(touchable->GetCopyNumber(depth), 0))};
        const auto physicalVolumeID{static_cast<std::uint32_t>(
            std::max(physicalVolume->GetInstanceID(), 0))};
        const auto volumeID{
            findVolumeID(physicalVolumeID, copyNo, parentVolumeID)};
        if (volumeID == InvalidID) {
            return InvalidID;
        }
        parentVolumeID = volumeID;
    }
    return parentVolumeID;
}

auto Geant4EventAdapter::MaterialIDFromVolume(std::uint32_t volumeID) const
    -> std::uint32_t {
    if (volumeID == InvalidID) {
        return InvalidID;
    }
    const auto* volume{fBatchScheduler->ExportedScene().FindVolume(volumeID)};
    if (volume == nullptr) {
        G4ExceptionDescription description{};
        description << "volume " << volumeID
                    << " is missing from the exported optical scene";
        G4Exception("Geant4EventAdapter::MaterialIDFromVolume",
                    "G4GOSceneMaterialLookup", FatalException, description);
        return InvalidID;
    }
    return volume->fMaterialID;
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
    fRunPerformance.Accumulate(fEventPerformance);
}

} // namespace G4GO::Optical
