#include "g4go/optical/Scene.hpp"

#include <algorithm>
#include <utility>

namespace G4GO::Optical {

auto PropertyTable::Constant() const -> bool {
    return fValues.size() == 1 ||
           (fValues.size() > 1 &&
            std::all_of(fValues.begin() + 1, fValues.end(), [&](auto value) {
                return value == fValues.front();
            }));
}

auto PropertyTable::Sample(float energyEv, float fallback) const -> float {
    if (fEnergyEv.empty() || fValues.empty()) {
        return fallback;
    }
    if (fEnergyEv.size() != fValues.size()) {
        return fallback;
    }
    if (fEnergyEv.size() == 1 || energyEv <= fEnergyEv.front()) {
        return fValues.front();
    }
    if (energyEv >= fEnergyEv.back()) {
        return fValues.back();
    }

    const auto upper{std::upper_bound(fEnergyEv.begin(), fEnergyEv.end(),
                                      energyEv)};
    const auto index{static_cast<std::size_t>(
        std::distance(fEnergyEv.begin(), upper) - 1)};
    const auto energy0{fEnergyEv[index]};
    const auto energy1{fEnergyEv[index + 1]};
    const auto value0{fValues[index]};
    const auto value1{fValues[index + 1]};
    const auto fraction{(energyEv - energy0) / (energy1 - energy0)};
    return value0 + fraction * (value1 - value0);
}

auto Scene::AddMaterial(Material material) -> std::uint32_t {
    fMaterials.push_back(std::move(material));
    return static_cast<std::uint32_t>(fMaterials.size() - 1);
}

auto Scene::AddSurface(Surface surface) -> std::uint32_t {
    fSurfaces.push_back(std::move(surface));
    return static_cast<std::uint32_t>(fSurfaces.size() - 1);
}

auto Scene::AddGeometry(Geometry geometry) -> std::uint32_t {
    fGeometries.push_back(std::move(geometry));
    return static_cast<std::uint32_t>(fGeometries.size() - 1);
}

auto Scene::AddVolume(Volume volume) -> std::uint32_t {
    if (volume.fVolumeID == InvalidID) {
        volume.fVolumeID = static_cast<std::uint32_t>(fVolumes.size());
    }
    fVolumes.push_back(std::move(volume));
    return static_cast<std::uint32_t>(fVolumes.size() - 1);
}

auto Scene::AddSurfaceBinding(SurfaceBinding binding) -> void {
    fSurfaceBindings.push_back(binding);
}

auto Scene::FindMaterial(std::uint32_t materialID) const -> const Material* {
    if (materialID >= fMaterials.size()) {
        return nullptr;
    }
    return &fMaterials[materialID];
}

auto Scene::FindSurface(std::uint32_t surfaceID) const -> const Surface* {
    if (surfaceID >= fSurfaces.size()) {
        return nullptr;
    }
    return &fSurfaces[surfaceID];
}

auto Scene::FindGeometry(std::uint32_t geometryID) const -> const Geometry* {
    if (geometryID >= fGeometries.size()) {
        return nullptr;
    }
    return &fGeometries[geometryID];
}

auto Scene::FindVolume(std::uint32_t volumeID) const -> const Volume* {
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

auto Scene::SetVolumeMayHaveCoincidentBoundary(std::uint32_t volumeID,
                                               bool value) -> void {
    const auto volume{std::find_if(
        fVolumes.begin(), fVolumes.end(), [&](const auto& candidate) {
            return candidate.fVolumeID == volumeID;
        })};
    if (volume != fVolumes.end()) {
        volume->fMayHaveCoincidentBoundary = value;
    }
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

    const auto* toVolume{FindVolume(toVolumeID)};
    if (toVolume != nullptr && toVolume->fSkinSurfaceID != InvalidID) {
        return FindSurface(toVolume->fSkinSurfaceID);
    }

    const auto* fromVolume{FindVolume(fromVolumeID)};
    if (fromVolume != nullptr && fromVolume->fSkinSurfaceID != InvalidID) {
        return FindSurface(fromVolume->fSkinSurfaceID);
    }
    return nullptr;
}

} // namespace G4GO::Optical
