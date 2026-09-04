#include "g4go/optical/Scene.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace G4GO::Optical {

Scene::Scene() :
    fMaterials{},
    fSurfaces{},
    fGeometries{},
    fVolumes{},
    fSurfaceBindings{},
    fWorldVolumeID{InvalidID} {}

auto PropertyTable::Constant() const -> bool {
    return fValues.size() == 1 ||
           (fValues.size() > 1 &&
            std::all_of(fValues.begin() + 1, fValues.end(), [&](auto value) {
                return value == fValues.at(0);
            }));
}

auto PropertyTable::Sample(float energyEv, float defaultValue) const -> float {
    if (fEnergyEv.empty() || fValues.empty()) {
        return defaultValue;
    }
    if (fEnergyEv.size() != fValues.size()) {
        return defaultValue;
    }
    if (fEnergyEv.size() == 1 || energyEv <= fEnergyEv.at(0)) {
        return fValues.at(0);
    }
    if (energyEv >= fEnergyEv.at(fEnergyEv.size() - 1U)) {
        return fValues.at(fValues.size() - 1U);
    }

    const auto upper{std::upper_bound(fEnergyEv.begin(), fEnergyEv.end(),
                                      energyEv)};
    const auto index{static_cast<std::size_t>(
        std::distance(fEnergyEv.begin(), upper) - 1)};
    const auto energy0{fEnergyEv.at(index)};
    const auto energy1{fEnergyEv.at(index + 1)};
    const auto value0{fValues.at(index)};
    const auto value1{fValues.at(index + 1)};
    const auto fraction{(energyEv - energy0) / (energy1 - energy0)};
    return value0 + fraction * (value1 - value0);
}

auto Scene::AddMaterial(Material material) -> std::uint32_t {
    fMaterials.emplace_back(std::move(material));
    return static_cast<std::uint32_t>(fMaterials.size() - 1);
}

auto Scene::AddSurface(Surface surface) -> std::uint32_t {
    fSurfaces.emplace_back(std::move(surface));
    return static_cast<std::uint32_t>(fSurfaces.size() - 1);
}

auto Scene::AddGeometry(Geometry geometry) -> std::uint32_t {
    fGeometries.emplace_back(std::move(geometry));
    return static_cast<std::uint32_t>(fGeometries.size() - 1);
}

auto Scene::AddVolume(Volume volume) -> std::uint32_t {
    if (volume.fVolumeID == InvalidID) {
        volume.fVolumeID = static_cast<std::uint32_t>(fVolumes.size());
    }
    fVolumes.emplace_back(std::move(volume));
    return static_cast<std::uint32_t>(fVolumes.size() - 1);
}

auto Scene::AddSurfaceBinding(SurfaceBinding binding) -> void {
    fSurfaceBindings.emplace_back(binding);
}

auto Scene::FindGeometry(std::uint32_t geometryID) -> Geometry* {
    if (geometryID >= fGeometries.size()) {
        return nullptr;
    }
    return &fGeometries.at(geometryID);
}

auto Scene::FindMaterial(std::uint32_t materialID) const -> const Material* {
    if (materialID >= fMaterials.size()) {
        return nullptr;
    }
    return &fMaterials.at(materialID);
}

auto Scene::FindSurface(std::uint32_t surfaceID) const -> const Surface* {
    if (surfaceID >= fSurfaces.size()) {
        return nullptr;
    }
    return &fSurfaces.at(surfaceID);
}

auto Scene::FindGeometry(std::uint32_t geometryID) const -> const Geometry* {
    if (geometryID >= fGeometries.size()) {
        return nullptr;
    }
    return &fGeometries.at(geometryID);
}

auto Scene::FindVolume(std::uint32_t volumeID) const -> const Volume* {
    if (volumeID < fVolumes.size() &&
        fVolumes.at(volumeID).fVolumeID == volumeID) {
        return &fVolumes.at(volumeID);
    }
    const auto volume{std::find_if(
        fVolumes.begin(), fVolumes.end(), [&](const auto& candidate) {
            return candidate.fVolumeID == volumeID;
        })};
    return volume == fVolumes.end() ? nullptr : &*volume;
}

auto Scene::FindVolume(std::uint32_t physicalVolumeID,
                       std::uint32_t copyNo,
                       std::uint32_t parentVolumeID) const -> const Volume* {
    const auto volume{std::find_if(
        fVolumes.begin(), fVolumes.end(), [&](const auto& candidate) {
            return candidate.fPhysicalVolumeID == physicalVolumeID &&
                   candidate.fCopyNo == copyNo &&
                   candidate.fParentVolumeID == parentVolumeID;
        })};
    return volume == fVolumes.end() ? nullptr : &*volume;
}

auto Scene::VolumeMayHaveCoincidentBoundary(std::uint32_t volumeID,
                                            bool value) -> void {
    if (volumeID < fVolumes.size() &&
        fVolumes.at(volumeID).fVolumeID == volumeID) {
        fVolumes[volumeID].fMayHaveCoincidentBoundary = value;
        return;
    }
    const auto volume{std::find_if(
        fVolumes.begin(), fVolumes.end(), [&](const auto& candidate) {
            return candidate.fVolumeID == volumeID;
        })};
    if (volume != fVolumes.end()) {
        volume->fMayHaveCoincidentBoundary = value;
    }
}

auto Scene::EnsureUniqueGeometry(std::uint32_t volumeID) -> void {
    auto volumeIterator{std::find_if(
        fVolumes.begin(), fVolumes.end(), [&](const auto& candidate) {
            return candidate.fVolumeID == volumeID;
        })};
    if (volumeIterator == fVolumes.end()) {
        throw std::invalid_argument(
            "cannot make geometry unique for an unknown volume");
    }
    const auto geometryID{volumeIterator->fGeometryID};
    if (geometryID >= fGeometries.size()) {
        throw std::invalid_argument(
            "volume references an unknown geometry");
    }
    const auto references{std::count_if(
        fVolumes.begin(), fVolumes.end(), [&](const auto& candidate) {
            return candidate.fGeometryID == geometryID;
        })};
    if (references <= 1) {
        return;
    }
    auto geometry{fGeometries.at(geometryID)};
    const auto uniqueGeometryID{AddGeometry(std::move(geometry))};
    volumeIterator->fGeometryID = uniqueGeometryID;
}

auto Scene::FindBoundarySurface(std::uint32_t fromVolumeID,
                                std::uint32_t toVolumeID) const
    -> const Surface* {
    const auto binding{std::find_if(
        fSurfaceBindings.begin(), fSurfaceBindings.end(), [&](const auto& item) {
            return item.fFromVolumeID == fromVolumeID &&
                   item.fToVolumeID == toVolumeID;
        })};
    if (binding != fSurfaceBindings.end()) {
        return FindSurface(binding->fSurfaceID);
    }

    const auto* fromVolume{FindVolume(fromVolumeID)};
    const auto* toVolume{FindVolume(toVolumeID)};
    if (fromVolume == nullptr || toVolume == nullptr) {
        return nullptr;
    }

    const auto findSkinSurface{[&](const Volume* volume) -> const Surface* {
        if (volume == nullptr || volume->fSkinSurfaceID == InvalidID) {
            return nullptr;
        }
        return FindSurface(volume->fSkinSurfaceID);
    }};
    if (toVolume->fParentVolumeID == fromVolumeID) {
        if (const auto* surface{findSkinSurface(toVolume)};
            surface != nullptr) {
            return surface;
        }
        return findSkinSurface(fromVolume);
    }
    if (const auto* surface{findSkinSurface(fromVolume)};
        surface != nullptr) {
        return surface;
    }
    return findSkinSurface(toVolume);
}

} // namespace G4GO::Optical
