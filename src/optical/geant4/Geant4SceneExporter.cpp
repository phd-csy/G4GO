#include "Geant4SceneExporter.hpp"

#include "G4GeometryTolerance.hh"
#include "G4LogicalBorderSurface.hh"
#include "G4LogicalSkinSurface.hh"
#include "G4LogicalVolume.hh"
#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4OpticalSurface.hh"
#include "G4Polyhedron.hh"
#include "G4ReplicaNavigation.hh"
#include "G4SystemOfUnits.hh"
#include "G4ThreeVector.hh"
#include "G4VPVParameterisation.hh"
#include "G4VPhysicalVolume.hh"
#include "G4VSolid.hh"
#include "g4go/detector/SensorSD.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace G4GO::Optical {

namespace {

class SceneBuilder final {
public:
    explicit SceneBuilder(std::uint32_t meshRotationSteps) :
        fScene{},
        fMaterialIDs{},
        fSurfaceIDs{},
        fGeometryIDs{},
        fVolumeIDs{},
        fVolumeBounds{},
        fMeshRotationSteps{meshRotationSteps} {}

    auto Build(const G4VPhysicalVolume* world) -> Scene {
        if (world == nullptr) {
            throw std::invalid_argument("Geant4 scene export requires a world");
        }

        const auto previousRotationSteps{G4Polyhedron::GetNumberOfRotationSteps()};
        if (fMeshRotationSteps < 8 or fMeshRotationSteps > 4096) {
            throw std::invalid_argument("mesh rotation steps must be in [8, 4096]");
        }
        G4Polyhedron::SetNumberOfRotationSteps(static_cast<G4int>(fMeshRotationSteps));
        try {
            const auto worldVolumeID{AddVolume(world, {}, InvalidID, 0)};
            MarkCoincidentBoundaries();
            AddBorderSurfaces();
            fScene.WorldVolumeID(worldVolumeID);
        } catch (...) {
            G4Polyhedron::SetNumberOfRotationSteps(previousRotationSteps);
            throw;
        }
        G4Polyhedron::SetNumberOfRotationSteps(previousRotationSteps);
        return fScene;
    }

private:
    // Geant4 geometry is a physical-volume tree, so recursive traversal mirrors its ownership model.
    // NOLINTBEGIN(misc-no-recursion)
    auto AddVolume(const G4VPhysicalVolume* physicalVolume, const Transform& parentTransform,
                   std::uint32_t parentVolumeID, std::uint32_t depth) -> std::uint32_t {
        if (physicalVolume == nullptr) {
            return InvalidID;
        }
        if (physicalVolume->GetLogicalVolume() == nullptr) {
            throw std::runtime_error("physical volume has no logical volume");
        }
        if (physicalVolume->IsParameterised() or physicalVolume->IsReplicated()) {
            const auto multiplicity{physicalVolume->GetMultiplicity()};
            if (multiplicity <= 0) {
                throw std::runtime_error("parameterised or replica volume has no copies: " +
                                         std::string{physicalVolume->GetName()});
            }

            // Geant4 exposes replica transformations through a mutating API and requires restoring the volume.
            auto* mutableVolume{
                const_cast<G4VPhysicalVolume*>(physicalVolume)}; // NOLINT(cppcoreguidelines-pro-type-const-cast)
            const auto savedRotation{physicalVolume->GetObjectRotationValue()};
            const auto hadRotation{physicalVolume->GetObjectRotation() != nullptr};
            const auto savedTranslation{physicalVolume->GetObjectTranslation()};
            const auto savedCopyNo{physicalVolume->GetCopyNo()};
            G4ReplicaNavigation replicaNavigation{};
            auto* parameterisation{physicalVolume->IsParameterised() ? physicalVolume->GetParameterisation() : nullptr};
            std::uint32_t firstVolumeID{InvalidID};
            for (int copyNo{}; copyNo < multiplicity; copyNo++) {
                mutableVolume->SetCopyNo(copyNo);
                if (parameterisation != nullptr) {
                    parameterisation->ComputeTransformation(copyNo, mutableVolume);
                } else {
                    replicaNavigation.ComputeTransformation(copyNo, mutableVolume);
                }
                const auto* solid{physicalVolume->GetLogicalVolume()->GetSolid()};
                if (parameterisation != nullptr) {
                    if (const auto* parameterisedSolid{parameterisation->ComputeSolid(copyNo, mutableVolume)};
                        parameterisedSolid != nullptr) {
                        solid = parameterisedSolid;
                    }
                }
                const auto* material{physicalVolume->GetLogicalVolume()->GetMaterial()};
                if (parameterisation != nullptr) {
                    if (const auto* parameterisedMaterial{parameterisation->ComputeMaterial(copyNo, mutableVolume)};
                        parameterisedMaterial != nullptr) {
                        material = parameterisedMaterial;
                    }
                }
                const auto volumeID{AddVolumeInstance(physicalVolume, parentTransform, parentVolumeID, depth,
                                                      static_cast<std::uint32_t>(copyNo), solid, material)};
                if (firstVolumeID == InvalidID) {
                    firstVolumeID = volumeID;
                }
            }
            RestorePhysicalVolume(mutableVolume, savedRotation, savedTranslation, savedCopyNo, hadRotation);
            return firstVolumeID;
        }
        return AddVolumeInstance(physicalVolume, parentTransform, parentVolumeID, depth,
                                 static_cast<std::uint32_t>(std::max(physicalVolume->GetCopyNo(), 0)),
                                 physicalVolume->GetLogicalVolume()->GetSolid(),
                                 physicalVolume->GetLogicalVolume()->GetMaterial());
    }

    auto AllocateVolumeID() -> std::uint32_t {
        if (fScene.Volumes().size() >= InvalidID) {
            throw std::overflow_error("scene contains too many volumes");
        }
        return static_cast<std::uint32_t>(fScene.Volumes().size());
    }

    static auto RestorePhysicalVolume(G4VPhysicalVolume* physicalVolume, const G4RotationMatrix& rotation,
                                      const G4ThreeVector& translation, G4int copyNo, bool hadRotation) -> void {
        physicalVolume->SetCopyNo(copyNo);
        physicalVolume->SetTranslation(translation);
        if (not hadRotation) {
            physicalVolume->SetRotation(nullptr);
        } else if (physicalVolume->GetRotation() == nullptr) {
            physicalVolume->SetRotation(new G4RotationMatrix(rotation));
        } else {
            *physicalVolume->GetRotation() = rotation;
        }
    }

    auto AddVolumeInstance(const G4VPhysicalVolume* physicalVolume, const Transform& parentTransform,
                           std::uint32_t parentVolumeID, std::uint32_t depth, std::uint32_t copyNo,
                           const G4VSolid* solid, const G4Material* material) -> std::uint32_t {
        const auto transform{Compose(parentTransform, physicalVolume)};
        const auto* logicalVolume{physicalVolume->GetLogicalVolume()};

        const auto geometryID{AddGeometry(solid, true)};
        const auto materialID{AddMaterial(material)};

        Volume volume{};
        volume.name = physicalVolume->GetName();
        volume.volumeID = AllocateVolumeID();
        volume.physicalVolumeID = static_cast<std::uint32_t>(std::max(physicalVolume->GetInstanceID(), 0));
        volume.copyNo = copyNo;
        volume.geometryID = geometryID;
        volume.materialID = materialID;
        volume.parentVolumeID = parentVolumeID;
        volume.depth = depth;
        volume.transform = transform;
        const auto* sensitiveDetector{logicalVolume->GetSensitiveDetector()};
        if (dynamic_cast<const G4GO::Detector::SensorSD*>(sensitiveDetector) != nullptr) {
            volume.sensorID = volume.copyNo;
        }

        if (const auto* skinSurface{G4LogicalSkinSurface::GetSurface(logicalVolume)}; skinSurface != nullptr) {
            volume.skinSurfaceID = AddSurface(skinSurface->GetSurfaceProperty());
        }

        const auto volumeID{volume.volumeID};
        fVolumeIDs[physicalVolume].emplace_back(volumeID);
        fVolumeBounds.emplace(volumeID, MakeBounds(fScene.FindGeometry(geometryID), transform));
        fScene.AddVolume(std::move(volume));

        for (std::size_t i{}; i < logicalVolume->GetNoDaughters(); i++) {
            AddVolume(logicalVolume->GetDaughter(i), transform, volumeID, depth + 1);
        }
        return volumeID;
    }
    // NOLINTEND(misc-no-recursion)

    auto AddGeometry(const G4VSolid* solid, bool cache) -> std::uint32_t {
        if (solid == nullptr) {
            throw std::runtime_error("logical volume has no solid");
        }
        if (cache) {
            if (const auto found{fGeometryIDs.find(solid)}; found != fGeometryIDs.end()) {
                return found->second;
            }
        }

        auto* polyhedron{solid->CreatePolyhedron()};
        if (polyhedron == nullptr) {
            throw std::runtime_error("unable to create polyhedron for solid " + std::string{solid->GetName()});
        }

        Geometry geometry{};
        geometry.name = solid->GetName();
        geometry.mesh.name = solid->GetName();
        const auto facetCount{polyhedron->GetNoFacets()};
        for (int face{1}; face <= facetCount; face++) {
            G4int count{};
            std::array<G4Point3D, 16> points{};
            polyhedron->GetFacet(face, count, points.data());
            if (count < 3 or std::cmp_greater(count, points.size())) {
                delete polyhedron;
                throw std::runtime_error("invalid polyhedron facet for solid " + std::string{solid->GetName()});
            }

            const auto faceNormal{polyhedron->GetUnitNormal(face)};
            for (int point{1}; point + 1 < count; point++) {
                const auto& first{points.at(0)};
                auto second{points.at(point)};
                auto third{points.at(point + 1)};
                const auto edge0{second - first};
                const auto edge1{third - first};
                auto cross{edge0.cross(edge1)};
                if (cross.dot(faceNormal) < 0.0) {
                    std::swap(second, third);
                    cross = (second - first).cross(third - first);
                }
                if (not(cross.mag2() > 0.0)) {
                    delete polyhedron;
                    throw std::runtime_error("degenerate polyhedron facet for solid " + std::string{solid->GetName()});
                }
                const auto base{static_cast<std::uint32_t>(geometry.mesh.verticesMm.size())};
                geometry.mesh.verticesMm.insert(geometry.mesh.verticesMm.end(),
                                                {ToVector(first), ToVector(second), ToVector(third)});
                geometry.mesh.indices.insert(geometry.mesh.indices.end(), {base, base + 1, base + 2});
                geometry.mesh.triangleNormals.emplace_back(cross.unit());
                geometry.mesh.triangleFlags.emplace_back(0);
            }
        }
        delete polyhedron;

        if (geometry.mesh.indices.empty()) {
            throw std::runtime_error("polyhedron has no triangles for solid " + std::string{solid->GetName()});
        }
        const auto geometryID{fScene.AddGeometry(std::move(geometry))};
        if (cache) {
            fGeometryIDs.emplace(solid, geometryID);
        }
        return geometryID;
    }

    struct Bounds {
        G4ThreeVector min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                          std::numeric_limits<double>::max()};
        G4ThreeVector max{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                          std::numeric_limits<double>::lowest()};
    };

    static auto MakeBounds(const Geometry* geometry, const Transform& transform) -> Bounds {
        if (geometry == nullptr or geometry->mesh.verticesMm.empty()) {
            throw std::runtime_error("volume geometry has no mesh vertices");
        }
        Bounds bounds{};
        for (const auto& vertex : geometry->mesh.verticesMm) {
            const auto point{ToWorld(transform, vertex)};
            bounds.min.setX(std::min(bounds.min.x(), point.x()));
            bounds.min.setY(std::min(bounds.min.y(), point.y()));
            bounds.min.setZ(std::min(bounds.min.z(), point.z()));
            bounds.max.setX(std::max(bounds.max.x(), point.x()));
            bounds.max.setY(std::max(bounds.max.y(), point.y()));
            bounds.max.setZ(std::max(bounds.max.z(), point.z()));
        }
        return bounds;
    }

    static auto BoundsOverlap(const Bounds& first, const Bounds& second, double tolerance) -> bool {
        return first.min.x() <= second.max.x() + tolerance and second.min.x() <= first.max.x() + tolerance and
               first.min.y() <= second.max.y() + tolerance and second.min.y() <= first.max.y() + tolerance and
               first.min.z() <= second.max.z() + tolerance and second.min.z() <= first.max.z() + tolerance;
    }

    struct Point2 {
        double x{};
        double y{};
    };

    static auto Cross2(const Point2& first, const Point2& second) -> double {
        return first.x * second.y - first.y * second.x;
    }

    static auto Subtract(const Point2& first, const Point2& second) -> Point2 {
        return {first.x - second.x, first.y - second.y};
    }

    static auto TrianglePoints(const Geometry& geometry, const Volume& volume, std::size_t triangle)
        -> std::array<G4ThreeVector, 3> {
        const auto base{3U * triangle};
        return {
            ToWorld(volume.transform, geometry.mesh.verticesMm.at(geometry.mesh.indices.at(base))),
            ToWorld(volume.transform, geometry.mesh.verticesMm.at(geometry.mesh.indices.at(base + 1U))),
            ToWorld(volume.transform, geometry.mesh.verticesMm.at(geometry.mesh.indices.at(base + 2U))),
        };
    }

    static auto TriangleBounds(const std::array<G4ThreeVector, 3>& points) -> Bounds {
        Bounds bounds{};
        for (const auto& point : points) {
            bounds.min.setX(std::min(bounds.min.x(), point.x()));
            bounds.min.setY(std::min(bounds.min.y(), point.y()));
            bounds.min.setZ(std::min(bounds.min.z(), point.z()));
            bounds.max.setX(std::max(bounds.max.x(), point.x()));
            bounds.max.setY(std::max(bounds.max.y(), point.y()));
            bounds.max.setZ(std::max(bounds.max.z(), point.z()));
        }
        return bounds;
    }

    static auto Project(const G4ThreeVector& point, const G4ThreeVector& origin, const G4ThreeVector& axisX,
                        const G4ThreeVector& axisY) -> Point2 {
        const auto relative{point - origin};
        return {relative.dot(axisX), relative.dot(axisY)};
    }

    static auto PolygonArea(const std::vector<Point2>& polygon) -> double {
        if (polygon.size() < 3U) {
            return 0.0;
        }
        auto area{double{}};
        for (std::size_t i{}; i < polygon.size(); i++) {
            const auto next{(i + 1U) % polygon.size()};
            area += Cross2(polygon.at(i), polygon.at(next));
        }
        return 0.5 * area;
    }

    static auto IntersectTriangles(const std::array<Point2, 3>& subject, const std::array<Point2, 3>& clip) -> double {
        std::vector<Point2> clipPolygon{clip.begin(), clip.end()};
        if (PolygonArea(clipPolygon) < 0.0) {
            std::ranges::reverse(clipPolygon);
        }
        std::vector<Point2> polygon{subject.begin(), subject.end()};
        for (std::size_t i{}; i < clipPolygon.size(); i++) {
            if (polygon.empty()) {
                return 0.0;
            }
            const auto edgeEnd{(i + 1U) % clipPolygon.size()};
            const auto& edgeStart{clipPolygon.at(i)};
            const auto& edgeStop{clipPolygon.at(edgeEnd)};
            const auto edgeVector{Subtract(edgeStop, edgeStart)};
            const auto input{std::move(polygon)};
            polygon.clear();
            auto previous{input.back()};
            auto previousInside{Cross2(edgeVector, Subtract(previous, edgeStart)) >= -1.0e-12};
            for (const auto& current : input) {
                const auto currentInside{Cross2(edgeVector, Subtract(current, edgeStart)) >= -1.0e-12};
                if (currentInside != previousInside) {
                    const auto segment{Subtract(current, previous)};
                    const auto denominator{Cross2(edgeVector, segment)};
                    if (std::abs(denominator) > 1.0e-18) {
                        const auto fraction{Cross2(edgeVector, Subtract(edgeStart, previous)) / denominator};
                        polygon.emplace_back(
                            Point2{previous.x + fraction * segment.x, previous.y + fraction * segment.y});
                    }
                }
                if (currentInside) {
                    polygon.emplace_back(current);
                }
                previous = current;
                previousInside = currentInside;
            }
        }
        return std::abs(PolygonArea(polygon));
    }

    static auto TrianglesCoincident(const std::array<G4ThreeVector, 3>& first,
                                    const std::array<G4ThreeVector, 3>& second, bool allowSameOrientation,
                                    double tolerance) -> bool {
        const auto firstNormalVector{(first.at(1) - first.at(0)).cross(first.at(2) - first.at(0))};
        const auto secondNormalVector{(second.at(1) - second.at(0)).cross(second.at(2) - second.at(0))};
        if (not(firstNormalVector.mag2() > 0.0) or not(secondNormalVector.mag2() > 0.0)) {
            return false;
        }
        const auto firstNormal{firstNormalVector.unit()};
        const auto secondNormal{secondNormalVector.unit()};
        const auto normalDot{firstNormal.dot(secondNormal)};
        if (allowSameOrientation ? std::abs(normalDot) < 0.9999 : normalDot > -0.9999) {
            return false;
        }
        for (const auto& point : first) {
            if (std::abs((point - second.at(0)).dot(secondNormal)) > tolerance) {
                return false;
            }
        }
        for (const auto& point : second) {
            if (std::abs((point - first.at(0)).dot(firstNormal)) > tolerance) {
                return false;
            }
        }

        const auto helper{
            std::abs(firstNormal.x()) < 0.9 ? G4ThreeVector{1.0, 0.0, 0.0}
                : G4ThreeVector{0.0, 1.0, 0.0}
        };
        const auto axisX{firstNormal.cross(helper).unit()};
        const auto axisY{firstNormal.cross(axisX).unit()};
        const std::array<Point2, 3> firstProjected{Project(first.at(0), first.at(0), axisX, axisY),
                                                   Project(first.at(1), first.at(0), axisX, axisY),
                                                   Project(first.at(2), first.at(0), axisX, axisY)};
        const std::array<Point2, 3> secondProjected{Project(second.at(0), first.at(0), axisX, axisY),
                                                    Project(second.at(1), first.at(0), axisX, axisY),
                                                    Project(second.at(2), first.at(0), axisX, axisY)};
        return IntersectTriangles(firstProjected, secondProjected) > tolerance * tolerance;
    }

    auto MarkCoincidentTriangles(const Volume& first, const Volume& second, double tolerance) -> bool {
        auto* firstGeometry{fScene.FindGeometry(first.geometryID)};
        auto* secondGeometry{fScene.FindGeometry(second.geometryID)};
        if (firstGeometry == nullptr or secondGeometry == nullptr) {
            throw std::runtime_error("coincident volume references an unknown geometry");
        }
        const auto firstTriangleCount{firstGeometry->mesh.indices.size() / 3U};
        const auto secondTriangleCount{secondGeometry->mesh.indices.size() / 3U};
        if (firstGeometry->mesh.triangleFlags.size() != firstTriangleCount or
            secondGeometry->mesh.triangleFlags.size() != secondTriangleCount) {
            throw std::runtime_error("geometry triangle flag count does not match triangle count");
        }
        const auto parentChild{first.parentVolumeID == second.volumeID or second.parentVolumeID == first.volumeID};
        std::vector<std::pair<std::size_t, std::size_t>> contacts{};
        for (std::size_t i{}; i < firstTriangleCount; i++) {
            const auto firstPoints{TrianglePoints(*firstGeometry, first, i)};
            const auto firstTriangleBounds{TriangleBounds(firstPoints)};
            for (std::size_t secondTriangle{}; secondTriangle < secondTriangleCount; secondTriangle++) {
                const auto secondPoints{TrianglePoints(*secondGeometry, second, secondTriangle)};
                if (not BoundsOverlap(firstTriangleBounds, TriangleBounds(secondPoints), tolerance) or
                    not TrianglesCoincident(firstPoints, secondPoints, parentChild, tolerance)) {
                    continue;
                }
                contacts.emplace_back(i, secondTriangle);
            }
        }
        if (contacts.empty()) {
            return false;
        }

        fScene.EnsureUniqueGeometry(first.volumeID);
        fScene.EnsureUniqueGeometry(second.volumeID);
        firstGeometry = fScene.FindGeometry(first.geometryID);
        secondGeometry = fScene.FindGeometry(second.geometryID);
        if (firstGeometry == nullptr or secondGeometry == nullptr) {
            throw std::runtime_error("coincident volume references an unknown geometry");
        }
        for (const auto& [firstTriangle, secondTriangle] : contacts) {
            firstGeometry->mesh.triangleFlags[firstTriangle] |= 1U;
            secondGeometry->mesh.triangleFlags[secondTriangle] |= 1U;
        }
        return true;
    }

    auto MarkCoincidentBoundaries() -> void {
        const auto tolerance{static_cast<double>(G4GeometryTolerance::GetInstance()->GetSurfaceTolerance() / mm)};
        const auto triangleTolerance{tolerance};
        const auto& volumes{fScene.Volumes()};
        for (std::size_t first{}; first < volumes.size(); first++) {
            for (std::size_t second{first + 1}; second < volumes.size(); second++) {
                const auto sameParent{volumes.at(first).parentVolumeID != InvalidID and
                                      volumes.at(first).parentVolumeID == volumes.at(second).parentVolumeID};
                const auto parentChild{volumes.at(first).parentVolumeID == volumes.at(second).volumeID or
                                       volumes.at(second).parentVolumeID == volumes.at(first).volumeID};
                if (not sameParent and not parentChild) {
                    continue;
                }
                const auto firstBounds{fVolumeBounds.at(volumes.at(first).volumeID)};
                const auto secondBounds{fVolumeBounds.at(volumes.at(second).volumeID)};
                if (BoundsOverlap(firstBounds, secondBounds, tolerance)) {
                    if (not MarkCoincidentTriangles(volumes.at(first), volumes.at(second), triangleTolerance)) {
                        continue;
                    }
                    fScene.VolumeMayHaveCoincidentBoundary(volumes.at(first).volumeID, true);
                    fScene.VolumeMayHaveCoincidentBoundary(volumes.at(second).volumeID, true);
                }
            }
        }
    }

    auto AddMaterial(const G4Material* material) -> std::uint32_t {
        if (material == nullptr) {
            throw std::runtime_error("logical volume has no material");
        }
        if (const auto found{fMaterialIDs.find(material)}; found != fMaterialIDs.end()) {
            return found->second;
        }

        Material opticalMaterial{};
        opticalMaterial.name = material->GetName();
        const auto* table{material->GetMaterialPropertiesTable()};
        if (table != nullptr) {
            RejectUnsupportedProperties(
                table, {"RAYLEIGH", "MIEHG", "WLSABSLENGTH", "WLSCOMPONENT", "WLSABSLENGTH2", "WLSCOMPONENT2"},
                "material " + material->GetName());
            opticalMaterial.rindex = ReadProperty(table, "RINDEX", 1.0);
            if (not opticalMaterial.rindex.Empty()) {
                if (opticalMaterial.rindex.energyEv.size() != opticalMaterial.rindex.values.size()) {
                    throw std::runtime_error("material " + material->GetName() +
                                             " has mismatched RINDEX energy/value tables");
                }
                if (not std::ranges::all_of(opticalMaterial.rindex.energyEv,
                                            [](const auto energy) -> bool { return std::isfinite(energy); })) {
                    throw std::runtime_error("material " + material->GetName() + " has invalid RINDEX energies");
                }
                if (not std::ranges::all_of(opticalMaterial.rindex.values, [](const auto value) -> bool {
                        return std::isfinite(value) and value > 0.0F;
                    })) {
                    throw std::runtime_error("material " + material->GetName() + " has invalid RINDEX values");
                }
                opticalMaterial.rindexMax = *std::ranges::max_element(opticalMaterial.rindex.values);
            }
            opticalMaterial.groupVelocityMmPerNs = ReadProperty(table, "GROUPVEL", mm / ns);
            opticalMaterial.absLengthMm = ReadProperty(table, "ABSLENGTH", mm);
            opticalMaterial.scintillationSpectrum = {
                ReadProperty(table, "SCINTILLATIONCOMPONENT1", 1.0),
                ReadProperty(table, "SCINTILLATIONCOMPONENT2", 1.0),
                ReadProperty(table, "SCINTILLATIONCOMPONENT3", 1.0),
            };
        }
        const auto materialID{fScene.AddMaterial(std::move(opticalMaterial))};
        fMaterialIDs.emplace(material, materialID);
        return materialID;
    }

    auto AddSurface(const G4SurfaceProperty* property) -> std::uint32_t {
        if (property == nullptr) {
            throw std::runtime_error("logical surface has no surface property");
        }
        if (const auto found{fSurfaceIDs.find(property)}; found != fSurfaceIDs.end()) {
            return found->second;
        }

        const auto* opticalSurface{dynamic_cast<const G4OpticalSurface*>(property)};
        if (opticalSurface == nullptr) {
            throw std::runtime_error("G4GO requires G4OpticalSurface");
        }

        Surface surface{};
        surface.name = opticalSurface->GetName();
        switch (opticalSurface->GetType()) {
        case dielectric_metal:
            surface.type = SurfaceType::DielectricMetal;
            break;
        case dielectric_dielectric:
            surface.type = SurfaceType::DielectricDielectric;
            break;
        default:
            throw std::runtime_error("GPU mesh backend requires dielectric_metal or "
                                     "dielectric_dielectric optical surfaces");
        }

        switch (opticalSurface->GetModel()) {
        case glisur:
            surface.model = SurfaceModel::Glisur;
            break;
        case unified:
            surface.model = SurfaceModel::Unified;
            break;
        default:
            throw std::runtime_error("GPU mesh backend supports only glisur and unified "
                                     "optical surface models");
        }

        surface.modelValue = static_cast<float>(opticalSurface->GetModel() == glisur ? opticalSurface->GetPolish() :
                                                                                       opticalSurface->GetSigmaAlpha());
        if (not std::isfinite(surface.modelValue) or
            (opticalSurface->GetModel() == glisur and (surface.modelValue < 0.0F or surface.modelValue > 1.0F)) or
            (opticalSurface->GetModel() == unified and surface.modelValue < 0.0F)) {
            throw std::runtime_error("GPU mesh backend received an invalid surface roughness "
                                     "parameter for surface " +
                                     opticalSurface->GetName());
        }
        switch (opticalSurface->GetFinish()) {
        case polished:
            surface.finish = SurfaceFinish::Polished;
            break;
        case ground:
            surface.finish = SurfaceFinish::Ground;
            break;
        default:
            throw std::runtime_error("GPU mesh backend supports polished and ground finishes "
                                     "only; painted finishes are unsupported");
        }

        if (const auto* table{opticalSurface->GetMaterialPropertiesTable()}; table != nullptr) {
            RejectUnsupportedProperties(table,
                                        {"REALRINDEX", "IMAGINARYRINDEX", "COATEDRINDEX", "DICHROIC", "COATEDTHICKNESS",
                                         "COATEDFRUSTRATEDTRANSMISSION"},
                                        "surface " + opticalSurface->GetName());
            surface.reflectivity = ReadProperty(table, "REFLECTIVITY", 1.0);
            surface.efficiency = ReadProperty(table, "EFFICIENCY", 1.0);
            surface.transmittance = ReadProperty(table, "TRANSMITTANCE", 1.0);
            surface.rindex = ReadProperty(table, "RINDEX", 1.0);
            surface.specularLobe = ReadProperty(table, "SPECULARLOBECONSTANT", 1.0);
            surface.specularSpike = ReadProperty(table, "SPECULARSPIKECONSTANT", 1.0);
            surface.backscatter = ReadProperty(table, "BACKSCATTERCONSTANT", 1.0);
            ValidateProbability(surface.reflectivity, "REFLECTIVITY", opticalSurface->GetName());
            ValidateProbability(surface.transmittance, "TRANSMITTANCE", opticalSurface->GetName());
            ValidateProbability(surface.efficiency, "EFFICIENCY", opticalSurface->GetName());
            ValidateProbability(surface.specularLobe, "SPECULARLOBECONSTANT", opticalSurface->GetName());
            ValidateProbability(surface.specularSpike, "SPECULARSPIKECONSTANT", opticalSurface->GetName());
            ValidateProbability(surface.backscatter, "BACKSCATTERCONSTANT", opticalSurface->GetName());
            if (table->ConstPropertyExists("SURFACEROUGHNESS")) {
                const auto roughness{table->GetConstProperty("SURFACEROUGHNESS")};
                if (not std::isfinite(roughness) or roughness < 0.0) {
                    throw std::runtime_error("surface " + opticalSurface->GetName() + " has invalid SURFACEROUGHNESS");
                }
                surface.surfaceRoughness.energyEv.emplace_back(0.0F);
                surface.surfaceRoughness.values.emplace_back(static_cast<float>(roughness / mm));
            }
        }

        const auto surfaceID{fScene.AddSurface(std::move(surface))};
        fSurfaceIDs.emplace(property, surfaceID);
        return surfaceID;
    }

    auto AddBorderSurfaces() -> void {
        const auto* table{G4LogicalBorderSurface::GetSurfaceTable()};
        if (table == nullptr) {
            return;
        }
        for (const auto& [volumes, surface] : *table) {
            const auto first{fVolumeIDs.find(volumes.first)};
            const auto second{fVolumeIDs.find(volumes.second)};
            if (first == fVolumeIDs.end() or second == fVolumeIDs.end()) {
                throw std::runtime_error("border surface refers to a volume outside the world");
            }
            const auto surfaceID{AddSurface(surface->GetSurfaceProperty())};
            for (const auto firstID : first->second) {
                for (const auto secondID : second->second) {
                    fScene.AddSurfaceBinding({firstID, secondID, surfaceID});
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

    static auto ReadProperty(const G4MaterialPropertiesTable* table, const char* name, double unit) -> PropertyTable {
        PropertyTable property{};
        const auto* vector{table->GetProperty(name)};
        if (vector == nullptr) {
            if (not table->ConstPropertyExists(name)) {
                return property;
            }
            property.energyEv.emplace_back(0.0F);
            property.values.emplace_back(static_cast<float>(table->GetConstProperty(name) / unit));
            return property;
        }
        const auto length{vector->GetVectorLength()};
        property.energyEv.reserve(length);
        property.values.reserve(length);
        for (std::size_t i{}; i < length; i++) {
            property.energyEv.emplace_back(static_cast<float>(vector->Energy(i) / eV));
            property.values.emplace_back(static_cast<float>((*vector)[i] / unit));
        }
        return property;
    }

    static auto ValidateProbability(const PropertyTable& property, const char* propertyName, const G4String& owner)
        -> void {
        if (property.energyEv.size() != property.values.size()) {
            throw std::runtime_error("surface " + owner + " property " + propertyName +
                                     " has mismatched energy/value tables");
        }
        if (not std::ranges::all_of(property.values, [](const auto value) -> bool {
                return std::isfinite(value) and value >= 0.0F and value <= 1.0F;
            })) {
            throw std::runtime_error("surface " + owner + " property " + propertyName +
                                     " must be finite and within [0, 1]");
        }
    }

    static auto RejectUnsupportedProperties(const G4MaterialPropertiesTable* table,
                                            std::initializer_list<const char*> names, const G4String& owner) -> void {
        for (const auto* name : names) {
            if (table->GetProperty(name) != nullptr or table->ConstPropertyExists(name)) {
                throw std::runtime_error("GPU optical transport does not support " + owner + " property " + name);
            }
        }
    }

    static auto Compose(const Transform& parent, const G4VPhysicalVolume* physicalVolume) -> Transform {
        const auto rotation{physicalVolume->GetObjectRotationValue()};
        const Rotation localRotation{
            static_cast<float>(rotation.xx()), static_cast<float>(rotation.xy()), static_cast<float>(rotation.xz()),
            static_cast<float>(rotation.yx()), static_cast<float>(rotation.yy()), static_cast<float>(rotation.yz()),
            static_cast<float>(rotation.zx()), static_cast<float>(rotation.zy()), static_cast<float>(rotation.zz()),
        };
        const auto localTranslation{physicalVolume->GetObjectTranslation()};
        const G4ThreeVector translation{
            localTranslation.x() / mm,
            localTranslation.y() / mm,
            localTranslation.z() / mm,
        };
        const auto composedRotation{Multiply(parent.rotation, localRotation)};
        const auto translated{RotateToWorld(parent.rotation, translation)};
        return {
            composedRotation,
            {parent.translationMm.x() + translated.x(), parent.translationMm.y() + translated.y(),
              parent.translationMm.z() + translated.z()},
        };
    }

    static auto Multiply(const Rotation& left, const Rotation& right) -> Rotation {
        return {
            left.xx * right.xx + left.xy * right.yx + left.xz * right.zx,
            left.xx * right.xy + left.xy * right.yy + left.xz * right.zy,
            left.xx * right.xz + left.xy * right.yz + left.xz * right.zz,
            left.yx * right.xx + left.yy * right.yx + left.yz * right.zx,
            left.yx * right.xy + left.yy * right.yy + left.yz * right.zy,
            left.yx * right.xz + left.yy * right.yz + left.yz * right.zz,
            left.zx * right.xx + left.zy * right.yx + left.zz * right.zx,
            left.zx * right.xy + left.zy * right.yy + left.zz * right.zy,
            left.zx * right.xz + left.zy * right.yz + left.zz * right.zz,
        };
    }

    Scene fScene;
    std::unordered_map<const G4Material*, std::uint32_t> fMaterialIDs;
    std::unordered_map<const G4SurfaceProperty*, std::uint32_t> fSurfaceIDs;
    std::unordered_map<const G4VSolid*, std::uint32_t> fGeometryIDs;
    std::unordered_map<const G4VPhysicalVolume*, std::vector<std::uint32_t>> fVolumeIDs;
    std::unordered_map<std::uint32_t, Bounds> fVolumeBounds;
    std::uint32_t fMeshRotationSteps;
};

} // namespace

auto Geant4SceneExporter::Export(const G4VPhysicalVolume* world) const -> Scene {
    return SceneBuilder{fMeshRotationSteps}.Build(world);
}

} // namespace G4GO::Optical
