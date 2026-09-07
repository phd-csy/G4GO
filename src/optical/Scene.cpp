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
    return values.size() == 1 || (values.size() > 1 && std::all_of(values.begin() + 1, values.end(),
                                                                   [&](auto value) { return value == values.at(0); }));
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
    if (volume.volumeID == InvalidID) {
        volume.volumeID = static_cast<std::uint32_t>(fVolumes.size());
    }
    fVolumes.emplace_back(std::move(volume));
    return static_cast<std::uint32_t>(fVolumes.size() - 1);
}

auto Scene::AddSurfaceBinding(SurfaceBinding binding) -> void { fSurfaceBindings.emplace_back(binding); }

auto Scene::FindGeometry(std::uint32_t geometryID) -> Geometry* {
    if (geometryID >= fGeometries.size()) {
        return nullptr;
    }
    return &fGeometries.at(geometryID);
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
    if (volumeID < fVolumes.size() && fVolumes.at(volumeID).volumeID == volumeID) {
        return &fVolumes.at(volumeID);
    }
    const auto volume{std::find_if(fVolumes.begin(), fVolumes.end(),
                                   [&](const auto& candidate) { return candidate.volumeID == volumeID; })};
    return volume == fVolumes.end() ? nullptr : &*volume;
}

auto Scene::FindVolume(std::uint32_t physicalVolumeID, std::uint32_t copyNo,
                       std::uint32_t parentVolumeID) const -> const Volume* {
    const auto volume{std::find_if(fVolumes.begin(), fVolumes.end(), [&](const auto& candidate) {
        return candidate.physicalVolumeID == physicalVolumeID && candidate.copyNo == copyNo &&
               candidate.parentVolumeID == parentVolumeID;
    })};
    return volume == fVolumes.end() ? nullptr : &*volume;
}

auto Scene::VolumeMayHaveCoincidentBoundary(std::uint32_t volumeID, bool value) -> void {
    if (volumeID < fVolumes.size() && fVolumes.at(volumeID).volumeID == volumeID) {
        fVolumes[volumeID].mayHaveCoincidentBoundary = value;
        return;
    }
    const auto volume{std::find_if(fVolumes.begin(), fVolumes.end(),
                                   [&](const auto& candidate) { return candidate.volumeID == volumeID; })};
    if (volume != fVolumes.end()) {
        volume->mayHaveCoincidentBoundary = value;
    }
}

auto Scene::EnsureUniqueGeometry(std::uint32_t volumeID) -> void {
    auto volumeIterator{std::find_if(fVolumes.begin(), fVolumes.end(),
                                     [&](const auto& candidate) { return candidate.volumeID == volumeID; })};
    if (volumeIterator == fVolumes.end()) {
        throw std::invalid_argument("cannot make geometry unique for an unknown volume");
    }
    const auto geometryID{volumeIterator->geometryID};
    if (geometryID >= fGeometries.size()) {
        throw std::invalid_argument("volume references an unknown geometry");
    }
    const auto references{std::count_if(fVolumes.begin(), fVolumes.end(),
                                        [&](const auto& candidate) { return candidate.geometryID == geometryID; })};
    if (references <= 1) {
        return;
    }
    auto geometry{fGeometries.at(geometryID)};
    const auto uniqueGeometryID{AddGeometry(std::move(geometry))};
    volumeIterator->geometryID = uniqueGeometryID;
}

auto Scene::FindBoundarySurface(std::uint32_t fromVolumeID, std::uint32_t toVolumeID) const -> const Surface* {
    const auto binding{std::find_if(fSurfaceBindings.begin(), fSurfaceBindings.end(), [&](const auto& item) {
        return item.fromVolumeID == fromVolumeID && item.toVolumeID == toVolumeID;
    })};
    if (binding != fSurfaceBindings.end()) {
        return FindSurface(binding->surfaceID);
    }

    const auto* fromVolume{FindVolume(fromVolumeID)};
    const auto* toVolume{FindVolume(toVolumeID)};
    if (fromVolume == nullptr || toVolume == nullptr) {
        return nullptr;
    }

    const auto findSkinSurface{[&](const Volume* volume) -> const Surface* {
        if (volume == nullptr || volume->skinSurfaceID == InvalidID) {
            return nullptr;
        }
        return FindSurface(volume->skinSurfaceID);
    }};
    if (toVolume->parentVolumeID == fromVolumeID) {
        if (const auto* surface{findSkinSurface(toVolume)}; surface != nullptr) {
            return surface;
        }
        return findSkinSurface(fromVolume);
    }
    if (const auto* surface{findSkinSurface(fromVolume)}; surface != nullptr) {
        return surface;
    }
    return findSkinSurface(toVolume);
}

} // namespace G4GO::Optical
