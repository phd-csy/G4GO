#include "G4GOSceneExporter.hpp"

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
        scene{},
        materialIDs{},
        surfaceIDs{},
        geometryIDs{},
        volumeIDs{},
        volumeBounds{},
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
            scene.WorldVolumeID(worldVolumeID);
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
            for (auto copyNo{int{}}; copyNo < multiplicity; ++copyNo) {
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
                    static_cast<std::uint32_t>(copyNo), solid, material)};
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
            physicalVolume->GetLogicalVolume()->GetMaterial());
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
                           const G4Material* material) -> std::uint32_t {
        const auto transform{Compose(parentTransform, physicalVolume)};
        const auto* logicalVolume{physicalVolume->GetLogicalVolume()};

        const auto geometryID{AddGeometry(solid, true)};
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

    static auto BoundsOverlap(const Bounds& first,
                              const Bounds& second,
                              double tolerance) -> bool {
        return first.fMin.x() <= second.fMax.x() + tolerance &&
               second.fMin.x() <= first.fMax.x() + tolerance &&
               first.fMin.y() <= second.fMax.y() + tolerance &&
               second.fMin.y() <= first.fMax.y() + tolerance &&
               first.fMin.z() <= second.fMax.z() + tolerance &&
               second.fMin.z() <= first.fMax.z() + tolerance;
    }

    struct Point2 {
        double fX{};
        double fY{};
    };

    static auto Cross2(const Point2& first, const Point2& second) -> double {
        return first.fX * second.fY - first.fY * second.fX;
    }

    static auto Subtract(const Point2& first, const Point2& second) -> Point2 {
        return {first.fX - second.fX, first.fY - second.fY};
    }

    static auto TrianglePoints(const Geometry& geometry,
                               const Volume& volume,
                               std::size_t triangle)
        -> std::array<G4ThreeVector, 3> {
        const auto base{3U * triangle};
        return {
            GeometryTransformer::ToWorld(
                volume.fTransform,
                geometry.fMesh.fVerticesMm.at(
                    geometry.fMesh.fIndices.at(base))),
            GeometryTransformer::ToWorld(
                volume.fTransform,
                geometry.fMesh.fVerticesMm.at(
                    geometry.fMesh.fIndices.at(base + 1U))),
            GeometryTransformer::ToWorld(
                volume.fTransform,
                geometry.fMesh.fVerticesMm.at(
                    geometry.fMesh.fIndices.at(base + 2U))),
        };
    }

    static auto TriangleBounds(const std::array<G4ThreeVector, 3>& points)
        -> Bounds {
        Bounds bounds{};
        for (const auto& point : points) {
            bounds.fMin.setX(std::min(bounds.fMin.x(), point.x()));
            bounds.fMin.setY(std::min(bounds.fMin.y(), point.y()));
            bounds.fMin.setZ(std::min(bounds.fMin.z(), point.z()));
            bounds.fMax.setX(std::max(bounds.fMax.x(), point.x()));
            bounds.fMax.setY(std::max(bounds.fMax.y(), point.y()));
            bounds.fMax.setZ(std::max(bounds.fMax.z(), point.z()));
        }
        return bounds;
    }

    static auto Project(const G4ThreeVector& point,
                        const G4ThreeVector& origin,
                        const G4ThreeVector& axisX,
                        const G4ThreeVector& axisY) -> Point2 {
        const auto relative{point - origin};
        return {relative.dot(axisX), relative.dot(axisY)};
    }

    static auto PolygonArea(const std::vector<Point2>& polygon) -> double {
        if (polygon.size() < 3U) {
            return 0.0;
        }
        auto area{double{}};
        for (auto index{std::size_t{}}; index < polygon.size(); ++index) {
            const auto next{(index + 1U) % polygon.size()};
            area += Cross2(polygon.at(index), polygon.at(next));
        }
        return 0.5 * area;
    }

    static auto IntersectTriangles(const std::array<Point2, 3>& subject,
                                   const std::array<Point2, 3>& clip)
        -> double {
        std::vector<Point2> clipPolygon{clip.begin(), clip.end()};
        if (PolygonArea(clipPolygon) < 0.0) {
            std::reverse(clipPolygon.begin(), clipPolygon.end());
        }
        std::vector<Point2> polygon{subject.begin(), subject.end()};
        for (auto edge{std::size_t{}}; edge < clipPolygon.size(); ++edge) {
            if (polygon.empty()) {
                return 0.0;
            }
            const auto edgeEnd{(edge + 1U) % clipPolygon.size()};
            const auto& edgeStart{clipPolygon.at(edge)};
            const auto& edgeStop{clipPolygon.at(edgeEnd)};
            const auto edgeVector{Subtract(edgeStop, edgeStart)};
            const auto input{std::move(polygon)};
            polygon.clear();
            auto previous{input.back()};
            auto previousInside{Cross2(edgeVector,
                                       Subtract(previous, edgeStart)) >=
                                -1.0e-12};
            for (const auto& current : input) {
                const auto currentInside{
                    Cross2(edgeVector, Subtract(current, edgeStart)) >=
                    -1.0e-12};
                if (currentInside != previousInside) {
                    const auto segment{Subtract(current, previous)};
                    const auto denominator{Cross2(edgeVector, segment)};
                    if (std::abs(denominator) > 1.0e-18) {
                        const auto fraction{
                            Cross2(edgeVector,
                                   Subtract(edgeStart, previous)) /
                            denominator};
                        polygon.emplace_back(
                            Point2{previous.fX + fraction * segment.fX,
                                   previous.fY + fraction * segment.fY});
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

    static auto TrianglesCoincident(
        const std::array<G4ThreeVector, 3>& first,
        const std::array<G4ThreeVector, 3>& second,
        bool allowSameOrientation,
        double tolerance) -> bool {
        const auto firstNormalVector{(first.at(1) - first.at(0)).cross(first.at(2) - first.at(0))};
        const auto secondNormalVector{(second.at(1) - second.at(0)).cross(second.at(2) - second.at(0))};
        if (!(firstNormalVector.mag2() > 0.0) ||
            !(secondNormalVector.mag2() > 0.0)) {
            return false;
        }
        const auto firstNormal{firstNormalVector.unit()};
        const auto secondNormal{secondNormalVector.unit()};
        const auto normalDot{firstNormal.dot(secondNormal)};
        if (allowSameOrientation ? std::abs(normalDot) < 0.9999 :
                                   normalDot > -0.9999) {
            return false;
        }
        for (const auto& point : first) {
            if (std::abs((point - second.at(0)).dot(secondNormal)) >
                tolerance) {
                return false;
            }
        }
        for (const auto& point : second) {
            if (std::abs((point - first.at(0)).dot(firstNormal)) >
                tolerance) {
                return false;
            }
        }

        const auto helper{
            std::abs(firstNormal.x()) < 0.9 ?
                G4ThreeVector{1.0, 0.0, 0.0}
                :
                G4ThreeVector{0.0, 1.0, 0.0}
        };
        const auto axisX{firstNormal.cross(helper).unit()};
        const auto axisY{firstNormal.cross(axisX).unit()};
        const std::array<Point2, 3> firstProjected{
            Project(first.at(0), first.at(0), axisX, axisY),
            Project(first.at(1), first.at(0), axisX, axisY),
            Project(first.at(2), first.at(0), axisX, axisY)};
        const std::array<Point2, 3> secondProjected{
            Project(second.at(0), first.at(0), axisX, axisY),
            Project(second.at(1), first.at(0), axisX, axisY),
            Project(second.at(2), first.at(0), axisX, axisY)};
        return IntersectTriangles(firstProjected, secondProjected) >
               tolerance * tolerance;
    }

    auto MarkCoincidentTriangles(const Volume& first,
                                 const Volume& second,
                                 double tolerance) -> bool {
        auto* firstGeometry{scene.FindGeometry(first.fGeometryID)};
        auto* secondGeometry{scene.FindGeometry(second.fGeometryID)};
        if (firstGeometry == nullptr || secondGeometry == nullptr) {
            throw std::runtime_error(
                "coincident volume references an unknown geometry");
        }
        const auto firstTriangleCount{firstGeometry->fMesh.fIndices.size() /
                                      3U};
        const auto secondTriangleCount{secondGeometry->fMesh.fIndices.size() /
                                       3U};
        if (firstGeometry->fMesh.fTriangleFlags.size() != firstTriangleCount ||
            secondGeometry->fMesh.fTriangleFlags.size() !=
                secondTriangleCount) {
            throw std::runtime_error(
                "geometry triangle flag count does not match triangle count");
        }
        const auto parentChild{
            first.fParentVolumeID == second.fVolumeID ||
            second.fParentVolumeID == first.fVolumeID};
        std::vector<std::pair<std::size_t, std::size_t>> contacts{};
        for (auto firstTriangle{std::size_t{}};
             firstTriangle < firstTriangleCount; ++firstTriangle) {
            const auto firstPoints{
                TrianglePoints(*firstGeometry, first, firstTriangle)};
            const auto firstTriangleBounds{TriangleBounds(firstPoints)};
            for (auto secondTriangle{std::size_t{}};
                 secondTriangle < secondTriangleCount; ++secondTriangle) {
                const auto secondPoints{
                    TrianglePoints(*secondGeometry, second, secondTriangle)};
                if (!BoundsOverlap(firstTriangleBounds,
                                   TriangleBounds(secondPoints), tolerance) ||
                    !TrianglesCoincident(firstPoints, secondPoints,
                                         parentChild, tolerance)) {
                    continue;
                }
                contacts.emplace_back(firstTriangle, secondTriangle);
            }
        }
        if (contacts.empty()) {
            return false;
        }

        scene.EnsureUniqueGeometry(first.fVolumeID);
        scene.EnsureUniqueGeometry(second.fVolumeID);
        firstGeometry = scene.FindGeometry(first.fGeometryID);
        secondGeometry = scene.FindGeometry(second.fGeometryID);
        if (firstGeometry == nullptr || secondGeometry == nullptr) {
            throw std::runtime_error(
                "coincident volume references an unknown geometry");
        }
        for (const auto& [firstTriangle, secondTriangle] : contacts) {
            firstGeometry->fMesh.fTriangleFlags[firstTriangle] |= 1U;
            secondGeometry->fMesh.fTriangleFlags[secondTriangle] |= 1U;
        }
        return true;
    }

    auto MarkCoincidentBoundaries() -> void {
        const auto tolerance{static_cast<double>(
            G4GeometryTolerance::GetInstance()->GetSurfaceTolerance() / mm)};
        const auto triangleTolerance{tolerance};
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
                if (BoundsOverlap(firstBounds, secondBounds, tolerance)) {
                    if (!MarkCoincidentTriangles(volumes.at(first),
                                                 volumes.at(second),
                                                 triangleTolerance)) {
                        continue;
                    }
                    scene.VolumeMayHaveCoincidentBoundary(
                        volumes.at(first).fVolumeID, true);
                    scene.VolumeMayHaveCoincidentBoundary(
                        volumes.at(second).fVolumeID, true);
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
            if (!opticalMaterial.fRindex.Empty()) {
                if (opticalMaterial.fRindex.fEnergyEv.size() !=
                    opticalMaterial.fRindex.fValues.size()) {
                    throw std::runtime_error(
                        "material " + material->GetName() +
                        " has mismatched RINDEX energy/value tables");
                }
                if (!std::all_of(
                        opticalMaterial.fRindex.fEnergyEv.begin(),
                        opticalMaterial.fRindex.fEnergyEv.end(),
                        [](const auto energy) { return std::isfinite(energy); })) {
                    throw std::runtime_error(
                        "material " + material->GetName() +
                        " has invalid RINDEX energies");
                }
                if (!std::all_of(
                        opticalMaterial.fRindex.fValues.begin(),
                        opticalMaterial.fRindex.fValues.end(),
                        [](const auto value) {
                            return std::isfinite(value) && value > 0.0F;
                        })) {
                    throw std::runtime_error(
                        "material " + material->GetName() +
                        " has invalid RINDEX values");
                }
                opticalMaterial.fRindexMax = *std::max_element(
                    opticalMaterial.fRindex.fValues.begin(),
                    opticalMaterial.fRindex.fValues.end());
            }
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
            opticalSurface->GetModel() == glisur ?
                opticalSurface->GetPolish() :
                opticalSurface->GetSigmaAlpha());
        if (!std::isfinite(surface.fModelValue) ||
            (opticalSurface->GetModel() == glisur &&
             (surface.fModelValue < 0.0F || surface.fModelValue > 1.0F)) ||
            (opticalSurface->GetModel() == unified &&
             surface.fModelValue < 0.0F)) {
            throw std::runtime_error(
                "GPU mesh backend received an invalid surface roughness "
                "parameter for surface " +
                opticalSurface->GetName());
        }
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
            RejectUnsupportedProperties(
                table,
                {"REALRINDEX", "IMAGINARYRINDEX", "COATEDRINDEX", "DICHROIC",
                 "COATEDTHICKNESS", "COATEDFRUSTRATEDTRANSMISSION"},
                "surface " + opticalSurface->GetName());
            surface.fReflectivity = ReadProperty(table, "REFLECTIVITY", 1.0);
            surface.fEfficiency = ReadProperty(table, "EFFICIENCY", 1.0);
            surface.fTransmittance = ReadProperty(table, "TRANSMITTANCE", 1.0);
            surface.fRindex = ReadProperty(table, "RINDEX", 1.0);
            surface.fSpecularLobe =
                ReadProperty(table, "SPECULARLOBECONSTANT", 1.0);
            surface.fSpecularSpike =
                ReadProperty(table, "SPECULARSPIKECONSTANT", 1.0);
            surface.fBackscatter =
                ReadProperty(table, "BACKSCATTERCONSTANT", 1.0);
            ValidateProbability(surface.fReflectivity, "REFLECTIVITY",
                                opticalSurface->GetName());
            ValidateProbability(surface.fTransmittance, "TRANSMITTANCE",
                                opticalSurface->GetName());
            ValidateProbability(surface.fEfficiency, "EFFICIENCY",
                                opticalSurface->GetName());
            ValidateProbability(surface.fSpecularLobe,
                                "SPECULARLOBECONSTANT",
                                opticalSurface->GetName());
            ValidateProbability(surface.fSpecularSpike,
                                "SPECULARSPIKECONSTANT",
                                opticalSurface->GetName());
            ValidateProbability(surface.fBackscatter,
                                "BACKSCATTERCONSTANT",
                                opticalSurface->GetName());
            if (table->ConstPropertyExists("SURFACEROUGHNESS")) {
                const auto roughness{table->GetConstProperty(
                    "SURFACEROUGHNESS")};
                if (!std::isfinite(roughness) || roughness < 0.0) {
                    throw std::runtime_error(
                        "surface " + opticalSurface->GetName() +
                        " has invalid SURFACEROUGHNESS");
                }
                surface.fSurfaceRoughness.fEnergyEv.emplace_back(0.0F);
                surface.fSurfaceRoughness.fValues.emplace_back(static_cast<float>(
                    roughness / mm));
            }
        }

        const auto surfaceID{scene.AddSurface(std::move(surface))};
        surfaceIDs.emplace(property, surfaceID);
        return surfaceID;
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
            if (!table->ConstPropertyExists(name)) {
                return property;
            }
            property.fEnergyEv.emplace_back(0.0F);
            property.fValues.emplace_back(static_cast<float>(
                table->GetConstProperty(name) / unit));
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

    static auto ValidateProbability(const PropertyTable& property,
                                    const char* propertyName,
                                    const G4String& owner) -> void {
        if (property.fEnergyEv.size() != property.fValues.size()) {
            throw std::runtime_error("surface " + owner + " property " +
                                     propertyName +
                                     " has mismatched energy/value tables");
        }
        if (!std::all_of(
                property.fValues.begin(), property.fValues.end(),
                [](const auto value) {
                    return std::isfinite(value) && value >= 0.0F &&
                           value <= 1.0F;
                })) {
            throw std::runtime_error("surface " + owner + " property " +
                                     propertyName +
                                     " must be finite and within [0, 1]");
        }
    }

    static auto RejectUnsupportedProperties(
        const G4MaterialPropertiesTable* table,
        std::initializer_list<const char*> names,
        const G4String& owner) -> void {
        for (const auto* name : names) {
            if (table->GetProperty(name) != nullptr ||
                table->ConstPropertyExists(name)) {
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

    Scene scene;
    std::unordered_map<const G4Material*, std::uint32_t> materialIDs;
    std::unordered_map<const G4SurfaceProperty*, std::uint32_t> surfaceIDs;
    std::unordered_map<const G4VSolid*, std::uint32_t> geometryIDs;
    std::unordered_map<const G4VPhysicalVolume*,
                       std::vector<std::uint32_t>>
        volumeIDs;
    std::unordered_map<std::uint32_t, Bounds> volumeBounds;
    std::uint32_t meshRotationSteps;
};

} // namespace

auto G4GOSceneExporter::Export(const G4VPhysicalVolume* world) const -> Scene {
    return SceneBuilder{fMeshRotationSteps}.Build(world);
}

} // namespace G4GO::Optical
