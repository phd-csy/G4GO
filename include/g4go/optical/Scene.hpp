#pragma once

#include "g4go/optical/Types.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace G4GO::Optical {

constexpr auto InvalidID{std::numeric_limits<std::uint32_t>::max()};

enum class SurfaceType : std::uint8_t {
    DielectricDielectric,
    DielectricMetal,
    DielectricLut,
    DielectricLutDavis,
    DielectricDichroic,
    Coated,
};

enum class SurfaceModel : std::uint8_t {
    Glisur,
    Unified,
    Lut,
    Davis,
    Dichroic,
};

enum class SurfaceFinish : std::uint8_t {
    Polished,
    PolishedFrontPainted,
    PolishedBackPainted,
    Ground,
    GroundFrontPainted,
    GroundBackPainted,
    Lut,
    Davis,
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
    SurfaceType fType{SurfaceType::DielectricDielectric};
    SurfaceModel fModel{SurfaceModel::Unified};
    SurfaceFinish fFinish{SurfaceFinish::Polished};
    float fModelValue{1.0F};
    PropertyTable fReflectivity{};
    PropertyTable fEfficiency{};
    PropertyTable fTransmittance{};
    PropertyTable fRindex{};
    PropertyTable fRealRindex{};
    PropertyTable fImaginaryRindex{};
    PropertyTable fCoatedRindex{};
    PropertyTable fSpecularLobe{};
    PropertyTable fSpecularSpike{};
    PropertyTable fBackscatter{};
    PropertyTable fSurfaceRoughness{};
    PropertyTable fDichroic{};
    float fCoatedThicknessMm{};
    bool fCoatedFrustratedTransmission{true};
};

struct MeshGeometry {
    std::string fName{};
    std::vector<Vector3> fVerticesMm{};
    std::vector<std::uint32_t> fIndices{};
    std::vector<std::uint8_t> fTriangleFlags{};
};

struct Geometry {
    std::string fName{};
    MeshGeometry fMesh{};
};

struct Volume {
    std::string fName{};
    std::uint32_t fVolumeID{InvalidID};
    std::uint32_t fPhysicalVolumeID{InvalidID};
    std::uint32_t fCopyNo{};
    std::uint32_t fGeometryID{InvalidID};
    std::uint32_t fMaterialID{InvalidID};
    std::uint32_t fParentVolumeID{InvalidID};
    std::uint32_t fSkinSurfaceID{InvalidID};
    std::uint32_t fSensorID{InvalidID};
    std::uint32_t fDepth{};
    bool fMayHaveCoincidentBoundary{};
    Transform fTransform{};
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
    auto AddGeometry(Geometry geometry) -> std::uint32_t;
    auto AddVolume(Volume volume) -> std::uint32_t;
    auto AddSurfaceBinding(SurfaceBinding binding) -> void;

    auto FindMaterial(std::uint32_t materialID) const -> const Material*;
    auto FindSurface(std::uint32_t surfaceID) const -> const Surface*;
    auto FindGeometry(std::uint32_t geometryID) const -> const Geometry*;
    auto FindVolume(std::uint32_t volumeID) const -> const Volume*;
    auto FindVolume(std::uint32_t physicalVolumeID,
                    std::uint32_t copyNo,
                    std::uint32_t parentVolumeID) const -> const Volume*;
    auto FindBoundarySurface(std::uint32_t fromVolumeID,
                             std::uint32_t toVolumeID) const
        -> const Surface*;

    auto Materials() const -> const std::vector<Material>& {
        return fMaterials;
    }
    auto Surfaces() const -> const std::vector<Surface>& { return fSurfaces; }
    auto Geometries() const -> const std::vector<Geometry>& {
        return fGeometries;
    }
    auto Volumes() const -> const std::vector<Volume>& { return fVolumes; }
    auto SurfaceBindings() const -> const std::vector<SurfaceBinding>& {
        return fSurfaceBindings;
    }
    auto WorldVolumeID() const -> std::uint32_t { return fWorldVolumeID; }
    auto SetWorldVolumeID(std::uint32_t volumeID) -> void {
        fWorldVolumeID = volumeID;
    }
    auto SetVolumeMayHaveCoincidentBoundary(std::uint32_t volumeID,
                                            bool value) -> void;

private:
    std::vector<Material> fMaterials{};
    std::vector<Surface> fSurfaces{};
    std::vector<Geometry> fGeometries{};
    std::vector<Volume> fVolumes{};
    std::vector<SurfaceBinding> fSurfaceBindings{};
    std::uint32_t fWorldVolumeID{InvalidID};
};

} // namespace G4GO::Optical
