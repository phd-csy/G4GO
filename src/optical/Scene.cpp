#include "g4go/optical/Scene.hpp"

#include <algorithm>
#include <ranges>
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
    return values.size() == 1 or
           (values.size() > 1 and std::ranges::all_of(values | std::views::drop(1),
                                                      [&](auto value) -> bool { return value == values.at(0); }));
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
    if (volumeID < fVolumes.size() and fVolumes.at(volumeID).volumeID == volumeID) {
        return &fVolumes.at(volumeID);
    }
    const auto volume{
        std::ranges::find_if(fVolumes, [&](const auto& candidate) -> bool { return candidate.volumeID == volumeID; })};
    return volume == fVolumes.end() ? nullptr : &*volume;
}

auto Scene::FindVolume(std::uint32_t physicalVolumeID, std::uint32_t copyNo, std::uint32_t parentVolumeID) const
    -> const Volume* {
    const auto volume{std::ranges::find_if(fVolumes, [&](const auto& candidate) -> bool {
        return candidate.physicalVolumeID == physicalVolumeID and candidate.copyNo == copyNo and
               candidate.parentVolumeID == parentVolumeID;
    })};
    return volume == fVolumes.end() ? nullptr : &*volume;
}

auto Scene::VolumeMayHaveCoincidentBoundary(std::uint32_t volumeID, bool value) -> void {
    if (volumeID < fVolumes.size() and fVolumes.at(volumeID).volumeID == volumeID) {
        fVolumes[volumeID].mayHaveCoincidentBoundary = value;
        return;
    }
    const auto volume{
        std::ranges::find_if(fVolumes, [&](const auto& candidate) -> bool { return candidate.volumeID == volumeID; })};
    if (volume != fVolumes.end()) {
        volume->mayHaveCoincidentBoundary = value;
    }
}

auto Scene::EnsureUniqueGeometry(std::uint32_t volumeID) -> void {
    auto volumeIterator{
        std::ranges::find_if(fVolumes, [&](const auto& candidate) -> bool { return candidate.volumeID == volumeID; })};
    if (volumeIterator == fVolumes.end()) {
        throw std::invalid_argument("cannot make geometry unique for an unknown volume");
    }
    const auto geometryID{volumeIterator->geometryID};
    if (geometryID >= fGeometries.size()) {
        throw std::invalid_argument("volume references an unknown geometry");
    }
    const auto references{std::ranges::count_if(
        fVolumes, [&](const auto& candidate) -> bool { return candidate.geometryID == geometryID; })};
    if (references <= 1) {
        return;
    }
    auto geometry{fGeometries.at(geometryID)};
    const auto uniqueGeometryID{AddGeometry(std::move(geometry))};
    volumeIterator->geometryID = uniqueGeometryID;
}

auto Scene::FindBoundarySurface(std::uint32_t fromVolumeID, std::uint32_t toVolumeID) const -> const Surface* {
    const auto binding{std::ranges::find_if(fSurfaceBindings, [&](const auto& item) -> bool {
        return item.fromVolumeID == fromVolumeID and item.toVolumeID == toVolumeID;
    })};
    if (binding != fSurfaceBindings.end()) {
        return FindSurface(binding->surfaceID);
    }

    const auto* fromVolume{FindVolume(fromVolumeID)};
    const auto* toVolume{FindVolume(toVolumeID)};
    if (fromVolume == nullptr or toVolume == nullptr) {
        return nullptr;
    }

    const auto findSkinSurface{[&](const Volume* volume) -> const Surface* {
        if (volume == nullptr or volume->skinSurfaceID == InvalidID) {
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
