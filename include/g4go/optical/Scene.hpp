#pragma once

#include "G4ThreeVector.hh"

#include <array>
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
    float xx{1.0F};
    float xy{};
    float xz{};
    float yx{};
    float yy{1.0F};
    float yz{};
    float zx{};
    float zy{};
    float zz{1.0F};
};

struct Transform {
    Rotation rotation{};
    G4ThreeVector translationMm{};
};

inline auto RotateToWorld(const Rotation& rotation, G4ThreeVector vector) -> G4ThreeVector {
    return {
        rotation.xx * vector.x() + rotation.xy * vector.y() + rotation.xz * vector.z(),
        rotation.yx * vector.x() + rotation.yy * vector.y() + rotation.yz * vector.z(),
        rotation.zx * vector.x() + rotation.zy * vector.y() + rotation.zz * vector.z(),
    };
}

inline auto ToWorld(const Transform& transform, G4ThreeVector point) -> G4ThreeVector {
    const auto rotated{RotateToWorld(transform.rotation, point)};
    return {rotated.x() + transform.translationMm.x(), rotated.y() + transform.translationMm.y(),
            rotated.z() + transform.translationMm.z()};
}

struct PropertyTable {
    std::vector<float> energyEv{};
    std::vector<float> values{};

    auto Empty() const -> bool { return energyEv.empty(); }
    auto Constant() const -> bool;
};

struct Material {
    std::string name{};
    PropertyTable rindex{};
    float rindexMax{1.0F};
    PropertyTable groupVelocityMmPerNs{};
    PropertyTable absLengthMm{};
    std::array<PropertyTable, 3> scintillationSpectrum{};
};

struct Surface {
    std::string name{};
    SurfaceType type{SurfaceType::DielectricDielectric};
    SurfaceModel model{SurfaceModel::Unified};
    SurfaceFinish finish{SurfaceFinish::Polished};
    float modelValue{1.0F};
    PropertyTable reflectivity{};
    PropertyTable efficiency{};
    PropertyTable transmittance{};
    PropertyTable rindex{};
    PropertyTable specularLobe{};
    PropertyTable specularSpike{};
    PropertyTable backscatter{};
    PropertyTable surfaceRoughness{};
};

struct MeshGeometry {
    std::string name{};
    std::vector<G4ThreeVector> verticesMm{};
    std::vector<std::uint32_t> indices{};
    std::vector<G4ThreeVector> triangleNormals{};
    std::vector<std::uint8_t> triangleFlags{};
};

struct Geometry {
    std::string name{};
    MeshGeometry mesh{};
};

struct Volume {
    std::string name{};
    std::uint32_t volumeID{InvalidID};
    std::uint32_t physicalVolumeID{InvalidID};
    std::uint32_t copyNo{};
    std::uint32_t geometryID{InvalidID};
    std::uint32_t materialID{InvalidID};
    std::uint32_t parentVolumeID{InvalidID};
    std::uint32_t skinSurfaceID{InvalidID};
    std::uint32_t sensorID{InvalidID};
    std::uint32_t depth{};
    bool mayHaveCoincidentBoundary{};
    Transform transform{};
};

struct SurfaceBinding {
    std::uint32_t fromVolumeID{InvalidID};
    std::uint32_t toVolumeID{InvalidID};
    std::uint32_t surfaceID{InvalidID};
};

class Scene final {
public:
    Scene();
    ~Scene() = default;

    auto AddMaterial(Material material) -> std::uint32_t;
    auto AddSurface(Surface surface) -> std::uint32_t;
    auto AddGeometry(Geometry geometry) -> std::uint32_t;
    auto AddVolume(Volume volume) -> std::uint32_t;
    auto AddSurfaceBinding(SurfaceBinding binding) -> void;

    auto FindGeometry(std::uint32_t geometryID) -> Geometry*;
    auto FindSurface(std::uint32_t surfaceID) const -> const Surface*;
    auto FindGeometry(std::uint32_t geometryID) const -> const Geometry*;
    auto FindVolume(std::uint32_t volumeID) const -> const Volume*;
    auto FindVolume(std::uint32_t physicalVolumeID, std::uint32_t copyNo,
                    std::uint32_t parentVolumeID) const -> const Volume*;
    auto FindBoundarySurface(std::uint32_t fromVolumeID, std::uint32_t toVolumeID) const -> const Surface*;

    auto Materials() const -> const std::vector<Material>& { return fMaterials; }
    auto Surfaces() const -> const std::vector<Surface>& { return fSurfaces; }
    auto Geometries() const -> const std::vector<Geometry>& { return fGeometries; }
    auto Volumes() const -> const std::vector<Volume>& { return fVolumes; }
    auto SurfaceBindings() const -> const std::vector<SurfaceBinding>& { return fSurfaceBindings; }
    auto WorldVolumeID() const -> std::uint32_t { return fWorldVolumeID; }
    auto WorldVolumeID(std::uint32_t volumeID) -> void { fWorldVolumeID = volumeID; }
    auto VolumeMayHaveCoincidentBoundary(std::uint32_t volumeID, bool value) -> void;
    auto EnsureUniqueGeometry(std::uint32_t volumeID) -> void;

private:
    std::vector<Material> fMaterials;
    std::vector<Surface> fSurfaces;
    std::vector<Geometry> fGeometries;
    std::vector<Volume> fVolumes;
    std::vector<SurfaceBinding> fSurfaceBindings;
    std::uint32_t fWorldVolumeID;
};

} // namespace G4GO::Optical
