#include "SceneExporter.hpp"

#include "G4Box.hh"
#include "G4LogicalBorderSurface.hh"
#include "G4LogicalSkinSurface.hh"
#include "G4LogicalSurface.hh"
#include "G4LogicalVolume.hh"
#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4MaterialPropertyVector.hh"
#include "G4OpticalSurface.hh"
#include "G4PVPlacement.hh"
#include "G4SystemOfUnits.hh"
#include "G4Tubs.hh"
#include "G4VPhysicalVolume.hh"
#include "g4go/optical/Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace G4GO::Optical {

namespace {

class SceneBuilder final {
public:
    auto Build(const G4VPhysicalVolume* world) -> Scene {
        if (world == nullptr) {
            throw std::invalid_argument("Geant4 scene export requires a world");
        }

        AddVolume(world, {}, 0);
        AddBorderSurfaces();
        scene.SetWorldVolumeID(world->GetInstanceID());
        return std::move(scene);
    }

private:
    auto AddVolume(const G4VPhysicalVolume* physicalVolume,
                   Transform parentTransform,
                   std::uint32_t depth) -> void {
        if (physicalVolume == nullptr) {
            return;
        }
        const auto transform{Compose(parentTransform, physicalVolume)};
        const auto* logicalVolume{physicalVolume->GetLogicalVolume()};
        if (logicalVolume == nullptr) {
            throw std::runtime_error("physical volume has no logical volume");
        }

        const auto materialID{AddMaterial(logicalVolume->GetMaterial())};
        auto solid{AddSolid(physicalVolume, logicalVolume->GetSolid(),
                            materialID, transform, depth)};

        if (const auto* skinSurface{
                G4LogicalSkinSurface::GetSurface(logicalVolume)};
            skinSurface != nullptr) {
            const auto surfaceID{
                AddSurface(skinSurface->GetSurfaceProperty())};
            solid.fSkinSurfaceID = surfaceID;
            if (const auto* surface{scene.FindSurface(surfaceID)};
                surface != nullptr && surface->fSensor) {
                solid.fSensorID = static_cast<std::uint32_t>(
                    std::max(physicalVolume->GetCopyNo(), 0));
            }
        }

        volumeIDs.emplace(physicalVolume, solid.fVolumeID);
        scene.AddSolid(std::move(solid));

        for (auto index{std::size_t{}};
             index < logicalVolume->GetNoDaughters(); ++index) {
            AddVolume(logicalVolume->GetDaughter(index), transform, depth + 1);
        }
    }

    auto AddMaterial(const G4Material* material) -> std::uint32_t {
        if (material == nullptr) {
            throw std::runtime_error("logical volume has no material");
        }
        if (const auto found{materialIDs.find(material)};
            found != materialIDs.end()) {
            return found->second;
        }

        Material opticalMaterial{};
        opticalMaterial.fName = material->GetName();
        const auto* table{material->GetMaterialPropertiesTable()};
        if (table != nullptr) {
            RejectUnsupportedProperties(
                table,
                {"RAYLEIGH", "MIEHG", "WLSABSLENGTH", "WLSCOMPONENT",
                 "WLSABSLENGTH2", "WLSCOMPONENT2"},
                "material " + material->GetName());
            opticalMaterial.fRindex = ReadProperty(table, "RINDEX", 1.0);
            opticalMaterial.fGroupVelocityMmPerNs =
                ReadProperty(table, "GROUPVEL", mm / ns);
            opticalMaterial.fAbsLengthMm = ReadProperty(table, "ABSLENGTH", mm);
        }
        const auto materialID{scene.AddMaterial(std::move(opticalMaterial))};
        materialIDs.emplace(material, materialID);
        return materialID;
    }

    auto AddSurface(const G4SurfaceProperty* property) -> std::uint32_t {
        if (property == nullptr) {
            throw std::runtime_error("logical surface has no surface property");
        }
        if (const auto found{surfaceIDs.find(property)};
            found != surfaceIDs.end()) {
            return found->second;
        }

        const auto* opticalSurface{
            dynamic_cast<const G4OpticalSurface*>(property)};
        if (opticalSurface == nullptr) {
            throw std::runtime_error("MVP requires G4OpticalSurface");
        }
        if (opticalSurface->GetModel() != unified &&
            opticalSurface->GetModel() != glisur) {
            throw std::runtime_error("MVP supports unified and glisur models only");
        }

        Surface surface{};
        surface.fName = opticalSurface->GetName();
        if (opticalSurface->GetType() == dielectric_metal) {
            surface.fKind = SurfaceKind::DielectricMetal;
        } else if (opticalSurface->GetType() == dielectric_dielectric) {
            surface.fKind = SurfaceKind::DielectricDielectric;
        } else {
            throw std::runtime_error("unsupported Geant4 optical surface type");
        }

        switch (opticalSurface->GetFinish()) {
        case polished:
        case polishedfrontpainted:
        case polishedbackpainted:
            surface.fFinish = SurfaceFinish::Polished;
            break;
        case ground:
        case groundfrontpainted:
        case groundbackpainted:
            surface.fFinish = SurfaceFinish::Ground;
            break;
        default:
            throw std::runtime_error(
                "unsupported Geant4 optical surface finish");
        }

        if (const auto* table{
                opticalSurface->GetMaterialPropertiesTable()};
            table != nullptr) {
            RejectUnsupportedProperties(
                table,
                {"TRANSMITTANCE", "SPECULARLOBECONSTANT",
                 "SPECULARSPIKECONSTANT", "BACKSCATTERCONSTANT", "RINDEX",
                 "REALRINDEX", "IMAGINARYRINDEX", "COATEDRINDEX"},
                "surface " + opticalSurface->GetName());
            surface.fReflectivity = ReadProperty(table, "REFLECTIVITY", 1.0);
            surface.fEfficiency = ReadProperty(table, "EFFICIENCY", 1.0);
        }
        surface.fSensor = std::any_of(
            surface.fEfficiency.fValues.begin(),
            surface.fEfficiency.fValues.end(),
            [](auto efficiency) { return efficiency > 0.0F; });

        const auto surfaceID{scene.AddSurface(std::move(surface))};
        surfaceIDs.emplace(property, surfaceID);
        return surfaceID;
    }

    auto AddSolid(const G4VPhysicalVolume* physicalVolume,
                  const G4VSolid* geometry,
                  std::uint32_t materialID,
                  Transform transform,
                  std::uint32_t depth) const -> Solid {
        if (geometry == nullptr) {
            throw std::runtime_error("logical volume has no solid");
        }

        Solid solid{};
        solid.fName = physicalVolume->GetName();
        solid.fVolumeID = static_cast<std::uint32_t>(
            std::max(physicalVolume->GetInstanceID(), 0));
        solid.fMaterialID = materialID;
        solid.fTransform = transform;
        solid.fDepth = depth;

        if (const auto* box{dynamic_cast<const G4Box*>(geometry)};
            box != nullptr) {
            solid.fKind = SolidKind::Box;
            solid.fHalfSizeMm = {
                static_cast<float>(box->GetXHalfLength() / mm),
                static_cast<float>(box->GetYHalfLength() / mm),
                static_cast<float>(box->GetZHalfLength() / mm),
            };
            return solid;
        }

        if (const auto* tub{dynamic_cast<const G4Tubs*>(geometry)};
            tub != nullptr) {
            if (std::abs(tub->GetDeltaPhiAngle() -
                         2.0 * std::numbers::pi_v<double>) >
                1.0e-9) {
                throw std::runtime_error("MVP supports full-phi G4Tubs only");
            }
            solid.fKind = SolidKind::Tub;
            solid.fInnerRadiusMm = static_cast<float>(tub->GetInnerRadius() / mm);
            solid.fOuterRadiusMm = static_cast<float>(tub->GetOuterRadius() / mm);
            solid.fHalfLengthMm = static_cast<float>(tub->GetZHalfLength() / mm);
            solid.fStartPhi = static_cast<float>(tub->GetStartPhiAngle());
            solid.fDeltaPhi = static_cast<float>(tub->GetDeltaPhiAngle());
            return solid;
        }

        throw std::runtime_error("MVP encountered an unsupported Geant4 solid");
    }

    auto AddBorderSurfaces() -> void {
        const auto* table{G4LogicalBorderSurface::GetSurfaceTable()};
        if (table == nullptr) {
            return;
        }
        for (const auto& [volumes, surface] : *table) {
            const auto first{volumeIDs.find(volumes.first)};
            const auto second{volumeIDs.find(volumes.second)};
            if (first == volumeIDs.end() || second == volumeIDs.end()) {
                throw std::runtime_error(
                    "border surface refers to a volume outside the world");
            }
            scene.AddSurfaceBinding({
                first->second,
                second->second,
                AddSurface(surface->GetSurfaceProperty()),
            });
        }
    }

    static auto ReadProperty(const G4MaterialPropertiesTable* table,
                             const char* name,
                             double unit) -> PropertyTable {
        PropertyTable property{};
        const auto* vector{table->GetProperty(name)};
        if (vector == nullptr) {
            return property;
        }
        const auto length{vector->GetVectorLength()};
        property.fEnergyEv.reserve(length);
        property.fValues.reserve(length);
        for (auto index{std::size_t{}}; index < length; ++index) {
            property.fEnergyEv.push_back(
                static_cast<float>(vector->Energy(index) / eV));
            property.fValues.push_back(
                static_cast<float>((*vector)[index] / unit));
        }
        return property;
    }

    static auto RejectUnsupportedProperties(
        const G4MaterialPropertiesTable* table,
        std::initializer_list<const char*> names,
        const G4String& owner) -> void {
        for (const auto* name : names) {
            if (table->GetProperty(name) != nullptr) {
                throw std::runtime_error(
                    "GPU optical transport does not support " + owner +
                    " property " + name);
            }
        }
    }

    static auto Compose(Transform parent,
                        const G4VPhysicalVolume* physicalVolume) -> Transform {
        const auto rotation{physicalVolume->GetObjectRotationValue()};
        const Rotation localRotation{
            static_cast<float>(rotation.xx()),
            static_cast<float>(rotation.xy()),
            static_cast<float>(rotation.xz()),
            static_cast<float>(rotation.yx()),
            static_cast<float>(rotation.yy()),
            static_cast<float>(rotation.yz()),
            static_cast<float>(rotation.zx()),
            static_cast<float>(rotation.zy()),
            static_cast<float>(rotation.zz()),
        };
        const auto localTranslation{physicalVolume->GetObjectTranslation()};
        const Vector3 translation{
            static_cast<float>(localTranslation.x() / mm),
            static_cast<float>(localTranslation.y() / mm),
            static_cast<float>(localTranslation.z() / mm),
        };
        const auto composedRotation{
            Multiply(parent.fRotation, localRotation)};
        const auto translated{RotateToWorld(parent.fRotation, translation)};
        return {
            composedRotation,
            {parent.fTranslationMm.fX + translated.fX,
              parent.fTranslationMm.fY + translated.fY,
              parent.fTranslationMm.fZ + translated.fZ},
        };
    }

    static auto Multiply(const Rotation& left, const Rotation& right)
        -> Rotation {
        return {
            left.fXX * right.fXX + left.fXY * right.fYX + left.fXZ * right.fZX,
            left.fXX * right.fXY + left.fXY * right.fYY + left.fXZ * right.fZY,
            left.fXX * right.fXZ + left.fXY * right.fYZ + left.fXZ * right.fZZ,
            left.fYX * right.fXX + left.fYY * right.fYX + left.fYZ * right.fZX,
            left.fYX * right.fXY + left.fYY * right.fYY + left.fYZ * right.fZY,
            left.fYX * right.fXZ + left.fYY * right.fYZ + left.fYZ * right.fZZ,
            left.fZX * right.fXX + left.fZY * right.fYX + left.fZZ * right.fZX,
            left.fZX * right.fXY + left.fZY * right.fYY + left.fZZ * right.fZY,
            left.fZX * right.fXZ + left.fZY * right.fYZ + left.fZZ * right.fZZ,
        };
    }

    Scene scene{};
    std::unordered_map<const G4Material*, std::uint32_t> materialIDs{};
    std::unordered_map<const G4SurfaceProperty*, std::uint32_t> surfaceIDs{};
    std::unordered_map<const G4VPhysicalVolume*, std::uint32_t> volumeIDs{};
};

} // namespace

auto Geant4SceneExporter::Export(const G4VPhysicalVolume* world) const -> Scene {
    return SceneBuilder{}.Build(world);
}

} // namespace G4GO::Optical
