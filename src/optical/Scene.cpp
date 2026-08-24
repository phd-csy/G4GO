#include "g4go/optical/Scene.hpp"

#include <algorithm>
#include <cmath>
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

auto Scene::AddSolid(Solid solid) -> std::uint32_t {
    if (solid.fVolumeID == InvalidID) {
        solid.fVolumeID = static_cast<std::uint32_t>(fSolids.size());
    }
    fSolids.push_back(std::move(solid));
    return static_cast<std::uint32_t>(fSolids.size() - 1);
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

auto Scene::FindSolid(std::uint32_t volumeID) const -> const Solid* {
    const auto solid{std::find_if(
        fSolids.begin(), fSolids.end(), [&](const auto& candidate) {
            return candidate.fVolumeID == volumeID;
        })};
    return solid == fSolids.end() ? nullptr : &*solid;
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

    const auto* toSolid{FindSolid(toVolumeID)};
    if (toSolid != nullptr && toSolid->fSkinSurfaceID != InvalidID) {
        return FindSurface(toSolid->fSkinSurfaceID);
    }

    const auto* fromSolid{FindSolid(fromVolumeID)};
    if (fromSolid != nullptr && fromSolid->fSkinSurfaceID != InvalidID) {
        return FindSurface(fromSolid->fSkinSurfaceID);
    }
    return nullptr;
}

} // namespace G4GO::Optical
