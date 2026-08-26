#pragma once

#include "g4go/optical/Types.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace G4GO::Optical {

constexpr auto InvalidID{std::numeric_limits<std::uint32_t>::max()};

enum class SolidKind : std::uint8_t {
    Box,
    Tub,
};

enum class SurfaceKind : std::uint8_t {
    DielectricDielectric,
    DielectricMetal,
};

enum class SurfaceFinish : std::uint8_t {
    Polished,
    Ground,
};

struct Rotation {
    float fXX{1.0F};
    float fXY{};
    float fXZ{};
    float fYX{};
    float fYY{1.0F};
    float fYZ{};
    float fZX{};
    float fZY{};
    float fZZ{1.0F};
};

struct Transform {
    Rotation fRotation{};
    Vector3 fTranslationMm{};
};

struct PropertyTable {
    std::vector<float> fEnergyEv{};
    std::vector<float> fValues{};

    auto Empty() const -> bool { return fEnergyEv.empty(); }
    auto Constant() const -> bool;
    auto Sample(float energyEv, float fallback) const -> float;
};

struct Material {
    std::string fName{};
    PropertyTable fRindex{};
    PropertyTable fGroupVelocityMmPerNs{};
    PropertyTable fAbsLengthMm{};
};

struct Surface {
    std::string fName{};
    SurfaceKind fKind{SurfaceKind::DielectricDielectric};
    SurfaceFinish fFinish{SurfaceFinish::Polished};
    bool fSensor{};
    PropertyTable fReflectivity{};
    PropertyTable fEfficiency{};
};

struct Solid {
    std::string fName{};
    SolidKind fKind{SolidKind::Box};
    Transform fTransform{};
    Vector3 fHalfSizeMm{};
    float fInnerRadiusMm{};
    float fOuterRadiusMm{};
    float fHalfLengthMm{};
    float fStartPhi{};
    float fDeltaPhi{};
    std::uint32_t fVolumeID{InvalidID};
    std::uint32_t fMaterialID{InvalidID};
    std::uint32_t fSkinSurfaceID{InvalidID};
    std::uint32_t fSensorID{InvalidID};
    std::uint32_t fDepth{};
};

struct SurfaceBinding {
    std::uint32_t fFromVolumeID{InvalidID};
    std::uint32_t fToVolumeID{InvalidID};
    std::uint32_t fSurfaceID{InvalidID};
};

class Scene final {
public:
    Scene() = default;
    ~Scene() = default;

    auto AddMaterial(Material material) -> std::uint32_t;
    auto AddSurface(Surface surface) -> std::uint32_t;
    auto AddSolid(Solid solid) -> std::uint32_t;
    auto AddSurfaceBinding(SurfaceBinding binding) -> void;

    auto FindMaterial(std::uint32_t materialID) const -> const Material*;
    auto FindSurface(std::uint32_t surfaceID) const -> const Surface*;
    auto FindSolid(std::uint32_t volumeID) const -> const Solid*;
    auto FindBoundarySurface(std::uint32_t fromVolumeID,
                             std::uint32_t toVolumeID) const
        -> const Surface*;

    auto Materials() const -> const std::vector<Material>& {
        return fMaterials;
    }
    auto Surfaces() const -> const std::vector<Surface>& { return fSurfaces; }
    auto Solids() const -> const std::vector<Solid>& { return fSolids; }
    auto SurfaceBindings() const -> const std::vector<SurfaceBinding>& {
        return fSurfaceBindings;
    }
    auto WorldVolumeID() const -> std::uint32_t { return fWorldVolumeID; }
    auto SetWorldVolumeID(std::uint32_t volumeID) -> void {
        fWorldVolumeID = volumeID;
    }

private:
    std::vector<Material> fMaterials{};
    std::vector<Surface> fSurfaces{};
    std::vector<Solid> fSolids{};
    std::vector<SurfaceBinding> fSurfaceBindings{};
    std::uint32_t fWorldVolumeID{InvalidID};
};

} // namespace G4GO::Optical
