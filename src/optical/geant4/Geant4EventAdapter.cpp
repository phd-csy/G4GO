#include "g4go/optical/geant4/Geant4EventAdapter.hpp"

#include "G4CerenkovQuasiTrackInfo.hh"
#include "G4Exception.hh"
#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4MaterialPropertyVector.hh"
#include "G4Navigator.hh"
#include "G4PhysicsModelCatalog.hh"
#include "G4SystemOfUnits.hh"
#include "G4TouchableHistory.hh"
#include "G4Track.hh"
#include "G4TransportationManager.hh"
#include "G4VPhysicalVolume.hh"
#include "G4VProcess.hh"
#include "g4go/optical/geant4/G4GOQuasiScintillationTrackInfo.hpp"
#include "g4go/optical/geant4/OpticalBatchScheduler.hpp"

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
    if (materialTable == nullptr or materialIndex >= materialTable->size()) {
        return nullptr;
    }
    return materialTable->at(materialIndex);
}

auto VectorFrom(const G4ThreeVector& vector) -> std::array<float, 3> {
    return {static_cast<float>(vector.x()), static_cast<float>(vector.y()), static_cast<float>(vector.z())};
}

auto VectorFromMm(const G4ThreeVector& vector) -> std::array<float, 3> {
    return {static_cast<float>(vector.x() / mm), static_cast<float>(vector.y() / mm),
            static_cast<float>(vector.z() / mm)};
}

} // namespace

Geant4EventAdapter::Geant4EventAdapter(PhotonTransportConfig configuration,
                                       std::shared_ptr<OpticalBatchScheduler> batchScheduler) :
    fRequestedBackend{configuration.backend},
    fConfiguration{configuration},
    fEmissions{},
    fVolumeIDCache{},
    fCachedLocatedVolume{nullptr},
    fCachedLocatedCopyNo{},
    fCachedVolumeID{InvalidID},
    fBatchScheduler{std::move(batchScheduler)},
    fEventStatistics{},
    fEventPerformance{},
    fRunStatistics{},
    fRunPerformance{},
    fEventID{-1},
    fNextPhotonID{},
    fEventPhotonCount{} {
    if (not fBatchScheduler) {
        throw std::invalid_argument("Geant4EventAdapter requires a batch scheduler");
    }
    fEmissions.reserve(std::min<std::size_t>(fConfiguration.maxPhotonsPerEvent, 1024));
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
    fEventPhotonCount = 0;
    fConfiguration.backend = fRequestedBackend;
    fBatchScheduler->BeginRun();
    fConfiguration.backend = fBatchScheduler->SelectedBackend();
}

auto Geant4EventAdapter::EndRun() -> void { fVolumeIDCache.clear(); }

auto Geant4EventAdapter::BeginEvent(G4int eventID) -> void {
    fEventID = eventID;
    fNextPhotonID = 0;
    fEventPhotonCount = 0;
    fEmissions.clear();
    fEventStatistics = {};
    fEventStatistics.validFields = StatisticFieldBit(PhotonTransportStatisticField::Generated);
    if (fConfiguration.backend == PhotonTransportBackend::OptiX) {
        fEventStatistics.validFields |= StatisticFieldBit(PhotonTransportStatisticField::Captured);
    }
    fEventPerformance = {};
}

auto Geant4EventAdapter::EndEvent() -> PhotonTransportFuture {
    if (fConfiguration.backend == PhotonTransportBackend::OptiX) {
        OpticalSubmission submission{};
        submission.eventID = static_cast<std::uint32_t>(fEventID);
        submission.emissions = std::move(fEmissions);
        submission.photonCount = fEventPhotonCount;
        submission.sourceStatistics = fEventStatistics;
        submission.sourcePerformance = fEventPerformance;
        auto future{fBatchScheduler->Submit(std::move(submission))};
        fRunPerformance.Accumulate(fEventPerformance);
        return future;
    }
    AccumulateEventStatistics();
    return {};
}

auto Geant4EventAdapter::SelectedBackend() const -> PhotonTransportBackend { return fConfiguration.backend; }

auto Geant4EventAdapter::RunStatistics() const -> const PhotonTransportStatistics& {
    if (fConfiguration.backend == PhotonTransportBackend::OptiX) {
        return fBatchScheduler->RunStatistics();
    }
    return fRunStatistics;
}

auto Geant4EventAdapter::ObserveGenerated(const G4Track& track) -> void {
    ++fEventStatistics.generatedCount;
    const auto* creatorProcess{track.GetCreatorProcess()};
    if (creatorProcess == nullptr) {
        return;
    }
    const auto& processName{creatorProcess->GetProcessName()};
    if (processName == "Cerenkov" or processName == "Cherenkov") {
        ++fEventPerformance.cerenkovPhotonCount;
    } else if (processName == "Scintillation") {
        ++fEventPerformance.scintillationPhotonCount;
    }
}

auto Geant4EventAdapter::Capture(const G4Track& track) -> void {
    const auto diagnostics{fConfiguration.enablePerformanceDiagnostics};
    const auto captureStart{diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};

    const auto& position{track.GetPosition()};
    const auto& direction{track.GetMomentumDirection()};
    const auto& polarization{track.GetPolarization()};

    OpticalEmission emission{};
    emission.positionMm = {
        static_cast<float>(position.x() / mm),
        static_cast<float>(position.y() / mm),
        static_cast<float>(position.z() / mm),
    };
    emission.timeNs = static_cast<float>(track.GetGlobalTime() / ns);
    emission.direction = {
        static_cast<float>(direction.x()),
        static_cast<float>(direction.y()),
        static_cast<float>(direction.z()),
    };
    emission.energyEv = static_cast<float>(track.GetKineticEnergy() / eV);
    emission.polarization = {
        static_cast<float>(polarization.x()),
        static_cast<float>(polarization.y()),
        static_cast<float>(polarization.z()),
    };
    emission.type = OpticalEmissionType::Direct;
    AppendOffloadedEmission(track, emission, 1, captureStart, "Geant4EventAdapter::Capture", "the creation");
}

auto Geant4EventAdapter::CaptureOffloaded(const G4Track& track) -> void {
    const auto appendTrackContext = [this, &track](G4ExceptionDescription& description) -> void {
        const auto* material{track.GetMaterial()};
        description << "event " << fEventID << ", track " << track.GetTrackID() << ", material ";
        if (material == nullptr) {
            description << "<null>";
        } else {
            description << material->GetName();
        }
    };
    const auto cerenkovModelID{G4PhysicsModelCatalog::GetModelID("model_QuasiCerenkov")};
    if (track.GetCreatorModelID() == cerenkovModelID) {
        if (const auto* info{G4CerenkovQuasiTrackInfo::Cast(track.GetAuxiliaryTrackInformation(cerenkovModelID))};
            info != nullptr) {
            CaptureCerenkov(track, *info);
            return;
        }
        G4ExceptionDescription description{};
        description << "quasi Cerenkov track has no recognized offload "
                       "metadata (";
        appendTrackContext(description);
        description << ")";
        G4Exception("Geant4EventAdapter::CaptureOffloaded", "G4GOCerenkovMetadata", FatalException, description);
        return;
    }

    const auto scintillationModelID{G4PhysicsModelCatalog::GetModelID("model_QuasiScintillation")};
    if (track.GetCreatorModelID() != scintillationModelID) {
        G4ExceptionDescription description{};
        description << "quasi scintillation track has unexpected creator "
                       "model ID "
                    << track.GetCreatorModelID() << " (";
        appendTrackContext(description);
        description << ")";
        G4Exception("Geant4EventAdapter::CaptureOffloaded", "G4GOScintillationModel", FatalException, description);
        return;
    }
    if (const auto* info{
            G4GOQuasiScintillationTrackInfo::Cast(track.GetAuxiliaryTrackInformation(scintillationModelID))};
        info != nullptr) {
        CaptureScintillation(track, *info);
        return;
    }

    G4ExceptionDescription description{};
    description << "quasi-optical track has no recognized offload metadata (";
    appendTrackContext(description);
    description << ")";
    G4Exception("Geant4EventAdapter::CaptureOffloaded", "G4GOUnknownOffloadTrack", FatalException, description);
}

auto Geant4EventAdapter::AppendOffloadedEmission(const G4Track& track, OpticalEmission emission,
                                                 std::size_t photonCount,
                                                 std::chrono::steady_clock::time_point captureStart,
                                                 const char* operation, const char* volumeDescription) -> bool {
    const auto maxPhotons{static_cast<std::uint64_t>(fConfiguration.maxPhotonsPerEvent)};
    if (photonCount > maxPhotons or fEventPhotonCount > maxPhotons - photonCount) {
        G4ExceptionDescription description{};
        description << "Optical photon limit reached in event " << fEventID
                    << ": limit=" << fConfiguration.maxPhotonsPerEvent;
        G4Exception(operation, "G4GOPhotonLimit", FatalException, description);
        return false;
    }

    const auto diagnostics{fConfiguration.enablePerformanceDiagnostics};
    const auto locateStart{diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    const auto* volume{LocateVolume(track)};
    if (diagnostics) {
        fEventPerformance.captureLocateVolumeMs +=
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - locateStart}.count();
    }
    if (volume == nullptr) {
        G4ExceptionDescription description{};
        description << "Unable to locate " << volumeDescription << " volume in event " << fEventID;
        G4Exception(operation, "G4GOVolumeLookup", FatalException, description);
        return false;
    }

    const auto mappingStart{diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    const auto volumeID{VolumeIDFromTrack(track, volume)};
    if (diagnostics) {
        fEventPerformance.captureVolumeMappingMs +=
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - mappingStart}.count();
    }
    if (not fBatchScheduler->ExportedScene().Volumes().empty() and volumeID == InvalidID) {
        G4ExceptionDescription description{};
        description << "Unable to map " << volumeDescription << " volume in event " << fEventID
                    << " to the exported scene";
        G4Exception(operation, "G4GOSceneVolumeLookup", FatalException, description);
        return false;
    }

    emission.eventID = static_cast<std::uint32_t>(fEventID);
    emission.firstPhotonID = fNextPhotonID;
    emission.photonCount = static_cast<std::uint32_t>(photonCount);
    emission.volumeID = volumeID;
    emission.materialID = MaterialIDFromVolume(volumeID);
    fNextPhotonID += static_cast<std::uint32_t>(photonCount);
    fEventPhotonCount += photonCount;
    fEmissions.emplace_back(emission);
    fEventStatistics.generatedCount += photonCount;
    fEventStatistics.capturedCount += photonCount;
    if (diagnostics) {
        fEventPerformance.captureTotalMs +=
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - captureStart}.count();
    }
    return true;
}

auto Geant4EventAdapter::CaptureCerenkov(const G4Track& track, const G4CerenkovQuasiTrackInfo& info) -> void {
    const auto diagnostics{fConfiguration.enablePerformanceDiagnostics};
    const auto captureStart{diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    const auto data{info.GetQuasiOpticalData()};
    const auto photonCount{data.num_photons > 0 ? static_cast<std::size_t>(data.num_photons) : std::size_t{}};
    if (photonCount == 0U) {
        return;
    }

    OpticalEmission emission{};
    emission.positionMm = VectorFromMm(track.GetPosition());
    emission.timeNs = static_cast<float>(track.GetGlobalTime() / ns);
    auto emissionDirection{data.delta_position};
    if (emissionDirection.mag2() > 0.0) {
        emissionDirection = emissionDirection.unit();
    } else {
        emissionDirection = track.GetMomentumDirection();
    }
    emission.direction = VectorFrom(emissionDirection);
    emission.stepLengthMm = static_cast<float>(data.step_length / mm);
    emission.stepDeltaMm = VectorFromMm(data.delta_position);
    emission.preVelocityMmPerNs = static_cast<float>(data.pre_velocity / (mm / ns));
    emission.deltaVelocityMmPerNs = static_cast<float>(data.delta_velocity / (mm / ns));
    emission.charge = static_cast<float>(data.charge);
    emission.preMeanPhotonCount = static_cast<float>(info.GetPreNumPhotons());
    emission.postMeanPhotonCount = static_cast<float>(info.GetPostNumPhotons());
    emission.type = OpticalEmissionType::Cerenkov;
    if (AppendOffloadedEmission(track, emission, photonCount, captureStart, "Geant4EventAdapter::CaptureCerenkov",
                                "Cerenkov offload")) {
        fEventPerformance.cerenkovPhotonCount += photonCount;
    }
}

auto Geant4EventAdapter::CaptureScintillation(const G4Track& track, const G4GOQuasiScintillationTrackInfo& info)
    -> void {
    const auto diagnostics{fConfiguration.enablePerformanceDiagnostics};
    const auto captureStart{diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    const auto data{info.QuasiOpticalData()};
    const auto photonCount{data.num_photons > 0 ? static_cast<std::size_t>(data.num_photons) : std::size_t{}};
    if (photonCount == 0U) {
        return;
    }

    const auto* material{MaterialFromIndex(data.mat_index)};
    const auto* properties{material == nullptr ? nullptr : material->GetMaterialPropertiesTable()};
    if (properties == nullptr) {
        G4ExceptionDescription description{};
        description << "scintillation offload metadata references material " << data.mat_index
                    << " without optical properties for " << "event " << fEventID << ", track " << track.GetTrackID();
        if (material != nullptr) {
            description << ", material " << material->GetName();
        }
        G4Exception("Geant4EventAdapter::CaptureScintillation", "G4GOScintillationProperties", FatalException,
                    description);
        return;
    }

    const auto componentIndex{info.ComponentIndex()};
    if (componentIndex < 0 or componentIndex > 2) {
        G4ExceptionDescription description{};
        description << "invalid scintillation component index " << componentIndex << " for event " << fEventID
                    << ", track " << track.GetTrackID() << ", material ";
        if (material == nullptr) {
            description << "<null>";
        } else {
            description << material->GetName();
        }
        description << " (index " << data.mat_index << ")";
        G4Exception("Geant4EventAdapter::CaptureScintillation", "G4GOScintillationComponent", FatalException,
                    description);
        return;
    }
    const std::array<const G4MaterialPropertyVector*, 3> components{properties->GetProperty(kSCINTILLATIONCOMPONENT1),
                                                                    properties->GetProperty(kSCINTILLATIONCOMPONENT2),
                                                                    properties->GetProperty(kSCINTILLATIONCOMPONENT3)};
    if (components.at(static_cast<std::size_t>(componentIndex)) == nullptr) {
        G4ExceptionDescription description{};
        description << "scintillation component " << componentIndex << " is missing for event " << fEventID
                    << ", track " << track.GetTrackID() << ", material ";
        if (material == nullptr) {
            description << "<null>";
        } else {
            description << material->GetName();
        }
        description << " (index " << data.mat_index << ")";
        G4Exception("Geant4EventAdapter::CaptureScintillation", "G4GOScintillationComponent", FatalException,
                    description);
        return;
    }
    OpticalEmission emission{};
    emission.positionMm = VectorFromMm(track.GetPosition());
    emission.timeNs = static_cast<float>(track.GetGlobalTime() / ns);
    emission.direction = VectorFrom(track.GetMomentumDirection());
    emission.stepLengthMm = static_cast<float>(data.step_length / mm);
    emission.stepDeltaMm = VectorFromMm(data.delta_position);
    emission.preVelocityMmPerNs = static_cast<float>(data.pre_velocity / (mm / ns));
    emission.deltaVelocityMmPerNs = static_cast<float>(data.delta_velocity / (mm / ns));
    emission.decayTimeNs = static_cast<float>(info.ScintillationTime() / ns);
    emission.riseTimeNs = static_cast<float>(info.RiseTime() / ns);
    emission.charge = static_cast<float>(data.charge);
    emission.spectrumID = static_cast<std::uint32_t>(componentIndex);
    emission.type = OpticalEmissionType::Scintillation;
    if (AppendOffloadedEmission(track, emission, photonCount, captureStart, "Geant4EventAdapter::CaptureScintillation",
                                "scintillation offload")) {
        fEventPerformance.scintillationPhotonCount += photonCount;
    }
}

auto Geant4EventAdapter::LocateVolume(const G4Track& track) -> const G4VPhysicalVolume* {
    if (const auto* volume{track.GetVolume()}; volume != nullptr) {
        return volume;
    }
    auto* navigator{G4TransportationManager::GetTransportationManager()->GetNavigatorForTracking()};
    const auto& position{track.GetPosition()};
    const auto& direction{track.GetMomentumDirection()};
    return navigator->LocateGlobalPointAndSetup(position, &direction, false);
}

auto Geant4EventAdapter::VolumeIDFromTrack(const G4Track& track, const G4VPhysicalVolume* locatedVolume) const
    -> std::uint32_t {
    const auto locatedID{static_cast<std::uint32_t>(std::max(locatedVolume->GetInstanceID(), 0))};
    if (fBatchScheduler->ExportedScene().Volumes().empty()) {
        return locatedID;
    }

    const auto& scene{fBatchScheduler->ExportedScene()};
    const auto locatedCopyNo{static_cast<std::uint32_t>(std::max(locatedVolume->GetCopyNo(), 0))};
    if (locatedVolume == fCachedLocatedVolume and locatedCopyNo == fCachedLocatedCopyNo and
        fCachedVolumeID != InvalidID) {
        return fCachedVolumeID;
    }

    const auto findVolumeID{
        [&](std::uint32_t physicalVolumeID, std::uint32_t copyNo, std::uint32_t parentVolumeID) -> std::uint32_t {
            const VolumeLookupKey key{physicalVolumeID, copyNo, parentVolumeID};
            if (const auto found{fVolumeIDCache.find(key)}; found != fVolumeIDCache.end()) {
                return found->second;
            }
            const auto* sceneVolume{scene.FindVolume(physicalVolumeID, copyNo, parentVolumeID)};
            const auto volumeID{sceneVolume == nullptr ? InvalidID : sceneVolume->volumeID};
            fVolumeIDCache.emplace(key, volumeID);
            return volumeID;
        }};
    const auto findUniqueVolumeID{[&](std::uint32_t physicalVolumeID, std::uint32_t copyNo) -> std::uint32_t {
        const VolumeLookupKey key{physicalVolumeID, copyNo, InvalidID};
        if (const auto found{fVolumeIDCache.find(key)}; found != fVolumeIDCache.end()) {
            return found->second;
        }
        std::uint32_t volumeID{InvalidID};
        for (const auto& candidate : scene.Volumes()) {
            if (candidate.physicalVolumeID != physicalVolumeID or candidate.copyNo != copyNo) {
                continue;
            }
            if (volumeID != InvalidID) {
                return InvalidID;
            }
            volumeID = candidate.volumeID;
        }
        if (volumeID != InvalidID) {
            fVolumeIDCache.emplace(key, volumeID);
        }
        return volumeID;
    }};

    if (const auto uniqueVolumeID{findUniqueVolumeID(locatedID, locatedCopyNo)}; uniqueVolumeID != InvalidID) {
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
        const auto copyNo{static_cast<std::uint32_t>(std::max(touchable->GetCopyNumber(depth), 0))};
        const auto physicalVolumeID{static_cast<std::uint32_t>(std::max(physicalVolume->GetInstanceID(), 0))};
        const auto volumeID{findVolumeID(physicalVolumeID, copyNo, parentVolumeID)};
        if (volumeID == InvalidID) {
            return InvalidID;
        }
        parentVolumeID = volumeID;
    }
    return parentVolumeID;
}

auto Geant4EventAdapter::MaterialIDFromVolume(std::uint32_t volumeID) const -> std::uint32_t {
    if (volumeID == InvalidID) {
        return InvalidID;
    }
    const auto* volume{fBatchScheduler->ExportedScene().FindVolume(volumeID)};
    if (volume == nullptr) {
        G4ExceptionDescription description{};
        description << "volume " << volumeID << " is missing from the exported optical scene";
        G4Exception("Geant4EventAdapter::MaterialIDFromVolume", "G4GOSceneMaterialLookup", FatalException, description);
        return InvalidID;
    }
    return volume->materialID;
}

auto Geant4EventAdapter::AccumulateEventStatistics() -> void {
    fRunStatistics.validFields |= fEventStatistics.validFields;
    fRunStatistics.generatedCount += fEventStatistics.generatedCount;
    fRunStatistics.capturedCount += fEventStatistics.capturedCount;
    fRunStatistics.detectedCount += fEventStatistics.detectedCount;
    fRunStatistics.absorbedCount += fEventStatistics.absorbedCount;
    fRunStatistics.escapedCount += fEventStatistics.escapedCount;
    fRunStatistics.truncatedCount += fEventStatistics.truncatedCount;
    fRunStatistics.maxBounceCount = std::max(fRunStatistics.maxBounceCount, fEventStatistics.maxBounceCount);
    fRunStatistics.invalidStateCount += fEventStatistics.invalidStateCount;
    fRunStatistics.zeroStepCount += fEventStatistics.zeroStepCount;
    fRunStatistics.transportTimeMs += fEventStatistics.transportTimeMs;
    fRunPerformance.Accumulate(fEventPerformance);
}

} // namespace G4GO::Optical
