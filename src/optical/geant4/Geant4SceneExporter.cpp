#include "Geant4SceneExporter.hpp"

#include "G4GeometryTolerance.hh"
#include "G4LogicalBorderSurface.hh"
#include "G4LogicalSkinSurface.hh"
#include "G4LogicalVolume.hh"
#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4MaterialPropertyVector.hh"
#include "G4OpticalSurface.hh"
#include "G4Polyhedron.hh"
#include "G4ReplicaNavigation.hh"
#include "G4SystemOfUnits.hh"
#include "G4ThreeVector.hh"
#include "G4VPVParameterisation.hh"
#include "G4VPhysicalVolume.hh"
#include "G4VSolid.hh"
#include "g4go/detector/SensorSD.hpp"
#include "g4go/optical/GeometryTransformer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace G4GO::Optical {

namespace {

class SceneBuilder final {
public:
    explicit SceneBuilder(std::uint32_t meshRotationSteps) :
        meshRotationSteps{meshRotationSteps} {}

    auto Build(const G4VPhysicalVolume* world) -> Scene {
        if (world == nullptr) {
            throw std::invalid_argument("Geant4 scene export requires a world");
        }

        const auto previousRotationSteps{
            G4Polyhedron::GetNumberOfRotationSteps()};
        if (meshRotationSteps < 8 || meshRotationSteps > 4096) {
            throw std::invalid_argument(
                "mesh rotation steps must be in [8, 4096]");
        }
        G4Polyhedron::SetNumberOfRotationSteps(
            static_cast<G4int>(meshRotationSteps));
        try {
            const auto worldVolumeID{AddVolume(world, {}, InvalidID, 0)};
            MarkCoincidentBoundaries();
            AddBorderSurfaces();
            scene.SetWorldVolumeID(worldVolumeID);
        } catch (...) {
            G4Polyhedron::SetNumberOfRotationSteps(previousRotationSteps);
            throw;
        }
        G4Polyhedron::SetNumberOfRotationSteps(previousRotationSteps);
        return scene;
    }

private:
    auto AddVolume(const G4VPhysicalVolume* physicalVolume,
                   Transform parentTransform,
                   std::uint32_t parentVolumeID,
                   std::uint32_t depth) -> std::uint32_t {
        if (physicalVolume == nullptr) {
            return InvalidID;
        }
        if (physicalVolume->GetLogicalVolume() == nullptr) {
            throw std::runtime_error("physical volume has no logical volume");
        }
        if (physicalVolume->IsParameterised() ||
            physicalVolume->IsReplicated()) {
            const auto multiplicity{physicalVolume->GetMultiplicity()};
            if (multiplicity <= 0) {
                throw std::runtime_error(
                    "parameterised or replica volume has no copies: " +
                    std::string{physicalVolume->GetName()});
            }

            auto* mutableVolume{const_cast<G4VPhysicalVolume*>(physicalVolume)};
            const auto savedRotation{physicalVolume->GetObjectRotationValue()};
            const auto hadRotation{physicalVolume->GetObjectRotation() != nullptr};
            const auto savedTranslation{physicalVolume->GetObjectTranslation()};
            const auto savedCopyNo{physicalVolume->GetCopyNo()};
            G4ReplicaNavigation replicaNavigation{};
            auto* parameterisation{
                physicalVolume->IsParameterised() ? physicalVolume->GetParameterisation() : nullptr};
            std::uint32_t firstVolumeID{InvalidID};
            for (auto copyNo{0}; copyNo < multiplicity; ++copyNo) {
                mutableVolume->SetCopyNo(copyNo);
                if (parameterisation != nullptr) {
                    parameterisation->ComputeTransformation(copyNo,
                                                            mutableVolume);
                } else {
                    replicaNavigation.ComputeTransformation(copyNo,
                                                            mutableVolume);
                }
                const auto* solid{physicalVolume->GetLogicalVolume()->GetSolid()};
                if (parameterisation != nullptr) {
                    if (const auto* parameterisedSolid{
                            parameterisation->ComputeSolid(copyNo,
                                                           mutableVolume)};
                        parameterisedSolid != nullptr) {
                        solid = parameterisedSolid;
                    }
                }
                const auto* material{
                    physicalVolume->GetLogicalVolume()->GetMaterial()};
                if (parameterisation != nullptr) {
                    if (const auto* parameterisedMaterial{
                            parameterisation->ComputeMaterial(copyNo,
                                                              mutableVolume)};
                        parameterisedMaterial != nullptr) {
                        material = parameterisedMaterial;
                    }
                }
                const auto volumeID{AddVolumeInstance(
                    physicalVolume, parentTransform, parentVolumeID, depth,
                    static_cast<std::uint32_t>(copyNo), solid, material,
                    true)};
                if (firstVolumeID == InvalidID) {
                    firstVolumeID = volumeID;
                }
            }
            RestorePhysicalVolume(mutableVolume, savedRotation, savedTranslation,
                                  savedCopyNo, hadRotation);
            return firstVolumeID;
        }
        return AddVolumeInstance(
            physicalVolume, parentTransform, parentVolumeID, depth,
            static_cast<std::uint32_t>(std::max(physicalVolume->GetCopyNo(), 0)),
            physicalVolume->GetLogicalVolume()->GetSolid(),
            physicalVolume->GetLogicalVolume()->GetMaterial(), false);
    }

    auto AllocateVolumeID() -> std::uint32_t {
        if (scene.Volumes().size() >= InvalidID) {
            throw std::overflow_error("scene contains too many volumes");
        }
        return static_cast<std::uint32_t>(scene.Volumes().size());
    }

    static auto RestorePhysicalVolume(G4VPhysicalVolume* physicalVolume,
                                      const G4RotationMatrix& rotation,
                                      const G4ThreeVector& translation,
                                      G4int copyNo,
                                      bool hadRotation) -> void {
        physicalVolume->SetCopyNo(copyNo);
        physicalVolume->SetTranslation(translation);
        if (!hadRotation) {
            physicalVolume->SetRotation(nullptr);
        } else if (physicalVolume->GetRotation() == nullptr) {
            physicalVolume->SetRotation(new G4RotationMatrix(rotation));
        } else {
            *physicalVolume->GetRotation() = rotation;
        }
    }

    auto AddVolumeInstance(const G4VPhysicalVolume* physicalVolume,
                           Transform parentTransform,
                           std::uint32_t parentVolumeID,
                           std::uint32_t depth,
                           std::uint32_t copyNo,
                           const G4VSolid* solid,
                           const G4Material* material,
                           bool forceUniqueGeometry) -> std::uint32_t {
        const auto transform{Compose(parentTransform, physicalVolume)};
        const auto* logicalVolume{physicalVolume->GetLogicalVolume()};

        const auto geometryID{AddGeometry(solid, !forceUniqueGeometry)};
        const auto materialID{AddMaterial(material)};

        Volume volume{};
        volume.fName = physicalVolume->GetName();
        volume.fVolumeID = AllocateVolumeID();
        volume.fPhysicalVolumeID = static_cast<std::uint32_t>(
            std::max(physicalVolume->GetInstanceID(), 0));
        volume.fCopyNo = copyNo;
        volume.fGeometryID = geometryID;
        volume.fMaterialID = materialID;
        volume.fParentVolumeID = parentVolumeID;
        volume.fDepth = depth;
        volume.fTransform = transform;
        const auto* sensitiveDetector{logicalVolume->GetSensitiveDetector()};
        if (dynamic_cast<const G4GO::Detector::SensorSD*>(
                sensitiveDetector) != nullptr) {
            volume.fSensorID = volume.fCopyNo;
        }

        if (const auto* skinSurface{
                G4LogicalSkinSurface::GetSurface(logicalVolume)};
            skinSurface != nullptr) {
            volume.fSkinSurfaceID =
                AddSurface(skinSurface->GetSurfaceProperty());
        }

        volumeIDs[physicalVolume].emplace_back(volume.fVolumeID);
        volumeBounds.emplace(volume.fVolumeID,
                             MakeBounds(scene.FindGeometry(geometryID),
                                        transform));
        scene.AddVolume(std::move(volume));

        for (auto index{std::size_t{}};
             index < logicalVolume->GetNoDaughters(); ++index) {
            AddVolume(logicalVolume->GetDaughter(index), transform,
                      volume.fVolumeID, depth + 1);
        }
        return volume.fVolumeID;
    }

    auto AddGeometry(const G4VSolid* solid, bool cache) -> std::uint32_t {
        if (solid == nullptr) {
            throw std::runtime_error("logical volume has no solid");
        }
        if (cache) {
            if (const auto found{geometryIDs.find(solid)};
                found != geometryIDs.end()) {
                return found->second;
            }
        }

        auto* polyhedron{solid->CreatePolyhedron()};
        if (polyhedron == nullptr) {
            throw std::runtime_error("unable to create polyhedron for solid " +
                                     std::string{solid->GetName()});
        }

        Geometry geometry{};
        geometry.fName = solid->GetName();
        geometry.fMesh.fName = solid->GetName();
        const auto facetCount{polyhedron->GetNoFacets()};
        for (auto face{1}; face <= facetCount; ++face) {
            G4int count{};
            std::array<G4Point3D, 16> points{};
            polyhedron->GetFacet(face, count, points.data());
            if (count < 3 || count > static_cast<G4int>(points.size())) {
                delete polyhedron;
                throw std::runtime_error("invalid polyhedron facet for solid " +
                                         std::string{solid->GetName()});
            }

            const auto faceNormal{polyhedron->GetUnitNormal(face)};
            for (auto point{1}; point + 1 < count; ++point) {
                const auto first{points.at(0)};
                auto second{points.at(point)};
                auto third{points.at(point + 1)};
                const auto edge0{second - first};
                const auto edge1{third - first};
                auto cross{edge0.cross(edge1)};
                if (cross.dot(faceNormal) < 0.0) {
                    std::swap(second, third);
                    cross = (second - first).cross(third - first);
                }
                if (!(cross.mag2() > 0.0)) {
                    delete polyhedron;
                    throw std::runtime_error(
                        "degenerate polyhedron facet for solid " +
                        std::string{solid->GetName()});
                }
                const auto base{static_cast<std::uint32_t>(
                    geometry.fMesh.fVerticesMm.size())};
                geometry.fMesh.fVerticesMm.insert(
                    geometry.fMesh.fVerticesMm.end(),
                    {ToVector(first), ToVector(second), ToVector(third)});
                geometry.fMesh.fIndices.insert(
                    geometry.fMesh.fIndices.end(), {base, base + 1, base + 2});
                geometry.fMesh.fTriangleNormals.emplace_back(cross.unit());
                geometry.fMesh.fTriangleFlags.emplace_back(0);
            }
        }
        delete polyhedron;

        if (geometry.fMesh.fIndices.empty()) {
            throw std::runtime_error("polyhedron has no triangles for solid " +
                                     std::string{solid->GetName()});
        }
        const auto geometryID{scene.AddGeometry(std::move(geometry))};
        if (cache) {
            geometryIDs.emplace(solid, geometryID);
        }
        return geometryID;
    }

    struct Bounds {
        G4ThreeVector fMin{std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::max()};
        G4ThreeVector fMax{std::numeric_limits<double>::lowest(),
                           std::numeric_limits<double>::lowest(),
                           std::numeric_limits<double>::lowest()};
    };

    static auto MakeBounds(const Geometry* geometry,
                           const Transform& transform) -> Bounds {
        if (geometry == nullptr || geometry->fMesh.fVerticesMm.empty()) {
            throw std::runtime_error("volume geometry has no mesh vertices");
        }
        Bounds bounds{};
        for (const auto& vertex : geometry->fMesh.fVerticesMm) {
            const auto point{GeometryTransformer::ToWorld(transform, vertex)};
            bounds.fMin.setX(std::min(bounds.fMin.x(), point.x()));
            bounds.fMin.setY(std::min(bounds.fMin.y(), point.y()));
            bounds.fMin.setZ(std::min(bounds.fMin.z(), point.z()));
            bounds.fMax.setX(std::max(bounds.fMax.x(), point.x()));
            bounds.fMax.setY(std::max(bounds.fMax.y(), point.y()));
            bounds.fMax.setZ(std::max(bounds.fMax.z(), point.z()));
        }
        return bounds;
    }

    static auto Overlap(double firstMin,
                        double firstMax,
                        double secondMin,
                        double secondMax,
                        double tolerance) -> bool {
        return std::min(firstMax, secondMax) + tolerance >=
               std::max(firstMin, secondMin);
    }

    static auto Touches(const Bounds& first,
                        const Bounds& second,
                        double tolerance) -> bool {
        const auto xOverlap{Overlap(first.fMin.x(), first.fMax.x(),
                                    second.fMin.x(), second.fMax.x(),
                                    tolerance)};
        const auto yOverlap{Overlap(first.fMin.y(), first.fMax.y(),
                                    second.fMin.y(), second.fMax.y(),
                                    tolerance)};
        const auto zOverlap{Overlap(first.fMin.z(), first.fMax.z(),
                                    second.fMin.z(), second.fMax.z(),
                                    tolerance)};
        return (xOverlap && yOverlap &&
                (std::abs(first.fMax.z() - second.fMin.z()) <= tolerance ||
                 std::abs(second.fMax.z() - first.fMin.z()) <= tolerance)) ||
               (xOverlap && zOverlap &&
                (std::abs(first.fMax.y() - second.fMin.y()) <= tolerance ||
                 std::abs(second.fMax.y() - first.fMin.y()) <= tolerance)) ||
               (yOverlap && zOverlap &&
                (std::abs(first.fMax.x() - second.fMin.x()) <= tolerance ||
                 std::abs(second.fMax.x() - first.fMin.x()) <= tolerance));
    }

    static auto Coordinate(const G4ThreeVector& point, int axis) -> double {
        return axis == 0 ? point.x() : axis == 1 ? point.y() :
                                                   point.z();
    }

    static auto TriangleTouchesPlane(const Geometry& geometry,
                                     std::size_t triangle,
                                     const Transform& transform,
                                     const Bounds& otherBounds,
                                     int axis,
                                     double plane,
                                     double tolerance) -> bool {
        const auto base{3U * triangle};
        const std::array<G4ThreeVector, 3> points{
            GeometryTransformer::ToWorld(
                transform,
                geometry.fMesh.fVerticesMm.at(
                    geometry.fMesh.fIndices.at(base))),
            GeometryTransformer::ToWorld(
                transform,
                geometry.fMesh.fVerticesMm.at(
                    geometry.fMesh.fIndices.at(base + 1U))),
            GeometryTransformer::ToWorld(
                transform,
                geometry.fMesh.fVerticesMm.at(
                    geometry.fMesh.fIndices.at(base + 2U)))};
        if (std::any_of(points.begin(), points.end(), [&](const auto& point) {
                return std::abs(Coordinate(point, axis) - plane) > tolerance;
            })) {
            return false;
        }

        for (auto projectedAxis{0}; projectedAxis < 3; ++projectedAxis) {
            if (projectedAxis == axis) {
                continue;
            }
            const auto triangleMin{std::min(
                {Coordinate(points.at(0), projectedAxis),
                 Coordinate(points.at(1), projectedAxis),
                 Coordinate(points.at(2), projectedAxis)})};
            const auto triangleMax{std::max(
                {Coordinate(points.at(0), projectedAxis),
                 Coordinate(points.at(1), projectedAxis),
                 Coordinate(points.at(2), projectedAxis)})};
            if (!Overlap(triangleMin, triangleMax,
                         Coordinate(otherBounds.fMin, projectedAxis),
                         Coordinate(otherBounds.fMax, projectedAxis),
                         tolerance)) {
                return false;
            }
        }
        return true;
    }

    static auto MarkTrianglesOnContactPlane(Geometry& geometry,
                                            const Volume& volume,
                                            const Bounds& otherBounds,
                                            int axis,
                                            double plane,
                                            double tolerance) -> void {
        const auto triangleCount{geometry.fMesh.fIndices.size() / 3U};
        if (geometry.fMesh.fTriangleFlags.size() != triangleCount) {
            throw std::runtime_error(
                "geometry triangle flag count does not match triangle count");
        }
        for (auto triangle{std::size_t{}}; triangle < triangleCount;
             ++triangle) {
            if (TriangleTouchesPlane(geometry, triangle, volume.fTransform,
                                     otherBounds, axis, plane, tolerance)) {
                geometry.fMesh.fTriangleFlags[triangle] |= 1U;
            }
        }
    }

    auto MarkCoincidentTriangles(const Volume& first,
                                 const Bounds& firstBounds,
                                 const Volume& second,
                                 const Bounds& secondBounds,
                                 double tolerance) -> void {
        auto* firstGeometry{scene.FindGeometry(first.fGeometryID)};
        auto* secondGeometry{scene.FindGeometry(second.fGeometryID)};
        if (firstGeometry == nullptr || secondGeometry == nullptr) {
            throw std::runtime_error(
                "coincident volume references an unknown geometry");
        }
        const auto markAxis{
            [&](int axis,
                double firstMin,
                double firstMax,
                double secondMin,
                double secondMax) {
                if (std::abs(firstMax - secondMin) <= tolerance) {
                    MarkTrianglesOnContactPlane(
                        *firstGeometry, first, secondBounds, axis, firstMax,
                        tolerance);
                    MarkTrianglesOnContactPlane(
                        *secondGeometry, second, firstBounds, axis, secondMin,
                        tolerance);
                }
                if (std::abs(secondMax - firstMin) <= tolerance) {
                    MarkTrianglesOnContactPlane(
                        *firstGeometry, first, secondBounds, axis, firstMin,
                        tolerance);
                    MarkTrianglesOnContactPlane(
                        *secondGeometry, second, firstBounds, axis, secondMax,
                        tolerance);
                }
            }};
        markAxis(0, firstBounds.fMin.x(), firstBounds.fMax.x(),
                 secondBounds.fMin.x(), secondBounds.fMax.x());
        markAxis(1, firstBounds.fMin.y(), firstBounds.fMax.y(),
                 secondBounds.fMin.y(), secondBounds.fMax.y());
        markAxis(2, firstBounds.fMin.z(), firstBounds.fMax.z(),
                 secondBounds.fMin.z(), secondBounds.fMax.z());
    }

    auto MarkCoincidentBoundaries() -> void {
        const auto tolerance{static_cast<double>(
            G4GeometryTolerance::GetInstance()->GetSurfaceTolerance() / mm)};
        const auto triangleTolerance{std::max(tolerance, 1.0e-5)};
        const auto& volumes{scene.Volumes()};
        for (auto first{std::size_t{}}; first < volumes.size(); ++first) {
            for (auto second{first + 1}; second < volumes.size(); ++second) {
                const auto sameParent{
                    volumes.at(first).fParentVolumeID != InvalidID &&
                    volumes.at(first).fParentVolumeID ==
                        volumes.at(second).fParentVolumeID};
                const auto parentChild{
                    volumes.at(first).fParentVolumeID ==
                        volumes.at(second).fVolumeID ||
                    volumes.at(second).fParentVolumeID ==
                        volumes.at(first).fVolumeID};
                if (!sameParent && !parentChild) {
                    continue;
                }
                const auto firstBounds{
                    volumeBounds.at(volumes.at(first).fVolumeID)};
                const auto secondBounds{
                    volumeBounds.at(volumes.at(second).fVolumeID)};
                if (Touches(firstBounds, secondBounds, tolerance)) {
                    scene.SetVolumeMayHaveCoincidentBoundary(
                        volumes.at(first).fVolumeID, true);
                    scene.SetVolumeMayHaveCoincidentBoundary(
                        volumes.at(second).fVolumeID, true);
                    MarkCoincidentTriangles(volumes.at(first), firstBounds,
                                            volumes.at(second), secondBounds,
                                            triangleTolerance);
                }
            }
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
            opticalMaterial.fScintillationSpectrum = {
                ReadProperty(table, "SCINTILLATIONCOMPONENT1", 1.0),
                ReadProperty(table, "SCINTILLATIONCOMPONENT2", 1.0),
                ReadProperty(table, "SCINTILLATIONCOMPONENT3", 1.0),
            };
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
            throw std::runtime_error("G4GO requires G4OpticalSurface");
        }

        Surface surface{};
        surface.fName = opticalSurface->GetName();
        switch (opticalSurface->GetType()) {
        case dielectric_metal:
            surface.fType = SurfaceType::DielectricMetal;
            break;
        case dielectric_dielectric:
            surface.fType = SurfaceType::DielectricDielectric;
            break;
        default:
            throw std::runtime_error(
                "GPU mesh backend requires dielectric_metal or "
                "dielectric_dielectric optical surfaces");
        }

        switch (opticalSurface->GetModel()) {
        case glisur:
            surface.fModel = SurfaceModel::Glisur;
            break;
        case unified:
            surface.fModel = SurfaceModel::Unified;
            break;
        default:
            throw std::runtime_error(
                "GPU mesh backend supports only glisur and unified "
                "optical surface models");
        }

        surface.fModelValue = static_cast<float>(
            opticalSurface->GetModel() == glisur ? opticalSurface->GetPolish() : opticalSurface->GetSigmaAlpha());
        switch (opticalSurface->GetFinish()) {
        case polished:
            surface.fFinish = SurfaceFinish::Polished;
            break;
        case ground:
            surface.fFinish = SurfaceFinish::Ground;
            break;
        default:
            throw std::runtime_error(
                "GPU mesh backend supports polished and ground finishes "
                "only; painted finishes are unsupported");
        }

        if (const auto* table{
                opticalSurface->GetMaterialPropertiesTable()};
            table != nullptr) {
            surface.fReflectivity = ReadProperty(table, "REFLECTIVITY", 1.0);
            surface.fEfficiency = ReadProperty(table, "EFFICIENCY", 1.0);
            surface.fTransmittance = ReadProperty(table, "TRANSMITTANCE", 1.0);
            surface.fRindex = ReadProperty(table, "RINDEX", 1.0);
            surface.fRealRindex = ReadProperty(table, "REALRINDEX", 1.0);
            surface.fImaginaryRindex =
                ReadProperty(table, "IMAGINARYRINDEX", 1.0);
            surface.fCoatedRindex = ReadProperty(table, "COATEDRINDEX", 1.0);
            surface.fSpecularLobe =
                ReadProperty(table, "SPECULARLOBECONSTANT", 1.0);
            surface.fSpecularSpike =
                ReadProperty(table, "SPECULARSPIKECONSTANT", 1.0);
            surface.fBackscatter =
                ReadProperty(table, "BACKSCATTERCONSTANT", 1.0);
            if (table->ConstPropertyExists("SURFACEROUGHNESS")) {
                surface.fSurfaceRoughness.fValues.emplace_back(static_cast<float>(
                    table->GetConstProperty("SURFACEROUGHNESS")));
            }
            if (table->ConstPropertyExists("COATEDTHICKNESS")) {
                surface.fCoatedThicknessMm = static_cast<float>(
                    table->GetConstProperty("COATEDTHICKNESS") / mm);
            }
            if (table->ConstPropertyExists(
                    "COATEDFRUSTRATEDTRANSMISSION")) {
                surface.fCoatedFrustratedTransmission =
                    table->GetConstProperty("COATEDFRUSTRATEDTRANSMISSION") !=
                    0.0;
            }
        }

        const auto surfaceID{scene.AddSurface(std::move(surface))};
        surfaceIDs.emplace(property, surfaceID);
        return surfaceID;
    }

    void AddBorderSurfaces() {
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
            const auto surfaceID{AddSurface(surface->GetSurfaceProperty())};
            for (const auto firstID : first->second) {
                for (const auto secondID : second->second) {
                    scene.AddSurfaceBinding({firstID, secondID, surfaceID});
                }
            }
        }
    }

    static auto ToVector(const G4Point3D& point) -> G4ThreeVector {
        return {
            point.x() / mm,
            point.y() / mm,
            point.z() / mm,
        };
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
            property.fEnergyEv.emplace_back(
                static_cast<float>(vector->Energy(index) / eV));
            property.fValues.emplace_back(
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
        const G4ThreeVector translation{
            localTranslation.x() / mm,
            localTranslation.y() / mm,
            localTranslation.z() / mm,
        };
        const auto composedRotation{
            Multiply(parent.fRotation, localRotation)};
        const auto translated{
            GeometryTransformer::RotateToWorld(parent.fRotation, translation)};
        return {
            composedRotation,
            {parent.fTranslationMm.x() + translated.x(),
              parent.fTranslationMm.y() + translated.y(),
              parent.fTranslationMm.z() + translated.z()},
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
    std::unordered_map<const G4VSolid*, std::uint32_t> geometryIDs{};
    std::unordered_map<const G4VPhysicalVolume*,
                       std::vector<std::uint32_t>>
        volumeIDs{};
    std::unordered_map<std::uint32_t, Bounds> volumeBounds{};
    std::uint32_t meshRotationSteps{};
};

} // namespace

auto Geant4SceneExporter::Export(const G4VPhysicalVolume* world) const -> Scene {
    return SceneBuilder{fMeshRotationSteps}.Build(world);
}

} // namespace G4GO::Optical
