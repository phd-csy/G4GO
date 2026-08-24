#include "g4go/optical/Geometry.hpp"
#include "g4go/optical/cpu/Transport.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

auto Expect(bool condition, std::string_view message) -> bool {
    if (condition) {
        return true;
    }
    std::cerr << "[g4go-test] " << message << '\n';
    return false;
}

auto MakeMaterial(std::string name, float index) -> G4GO::Optical::Material {
    G4GO::Optical::Material material{};
    material.fName = std::move(name);
    material.fRindex.fEnergyEv = {1.0F, 10.0F};
    material.fRindex.fValues = {index, index};
    return material;
}

auto MakeBox(std::string name,
             std::uint32_t volumeID,
             std::uint32_t materialID,
             G4GO::Optical::Vector3 halfSize,
             std::uint32_t depth) -> G4GO::Optical::Solid {
    G4GO::Optical::Solid solid{};
    solid.fName = std::move(name);
    solid.fKind = G4GO::Optical::SolidKind::Box;
    solid.fVolumeID = volumeID;
    solid.fMaterialID = materialID;
    solid.fHalfSizeMm = halfSize;
    solid.fDepth = depth;
    return solid;
}

auto TestGeometry() -> bool {
    using namespace G4GO::Optical;

    Scene scene{};
    const auto airID{scene.AddMaterial(MakeMaterial("air", 1.0F))};
    const auto coreID{scene.AddMaterial(MakeMaterial("core", 1.5F))};
    scene.AddSolid(MakeBox("world", 1, airID, {10.0F, 10.0F, 10.0F}, 0));
    scene.AddSolid(MakeBox("core", 2, coreID, {1.0F, 1.0F, 1.0F}, 1));
    scene.SetWorldVolumeID(1);

    if (!Expect(LocateVolume(scene, {0.0F, 0.0F, 0.0F}) == 2,
                "core should be selected as the deepest volume")) {
        return false;
    }
    if (!Expect(LocateVolume(scene, {2.0F, 0.0F, 0.0F}) == 1,
                "world should contain points outside the core")) {
        return false;
    }

    Photon photon{};
    photon.fPositionMm = {0.0F, 0.0F, 0.0F};
    photon.fDirection = {1.0F, 0.0F, 0.0F};
    photon.fEnergyEv = 3.0F;
    photon.fVolumeID = 2;
    const auto intersection{NextIntersection(scene, photon, 1.0e-4F)};
    return Expect(intersection && intersection->fVolumeID == 2 &&
                      std::abs(intersection->fDistanceMm - 1.0F) < 1.0e-5F,
                  "core boundary distance should be one millimetre");
}

auto TestAbsorption() -> bool {
    using namespace G4GO::Optical;

    Scene scene{};
    auto material{MakeMaterial("absorber", 1.0F)};
    material.fAbsLengthMm.fEnergyEv = {1.0F, 10.0F};
    material.fAbsLengthMm.fValues = {0.01F, 0.01F};
    const auto materialID{scene.AddMaterial(std::move(material))};
    scene.AddSolid(MakeBox("world", 1, materialID, {10.0F, 10.0F, 10.0F}, 0));
    scene.SetWorldVolumeID(1);

    Photon photon{};
    photon.fPositionMm = {0.0F, 0.0F, 0.0F};
    photon.fDirection = {1.0F, 0.0F, 0.0F};
    photon.fEnergyEv = 3.0F;
    photon.fVolumeID = 1;

    TransportConfig config{};
    config.fSeed = 1234;
    const auto result{CpuOpticalTransport{config}.Transport(scene, {&photon, 1})};
    return Expect(result.fStats.fAbsorbedCount == 1 &&
                      result.fStats.fEscapedCount == 0,
                  "short absorption length should terminate the photon");
}

auto TestSensor() -> bool {
    using namespace G4GO::Optical;

    Scene scene{};
    const auto airID{scene.AddMaterial(MakeMaterial("air", 1.0F))};
    const auto siliconID{scene.AddMaterial(MakeMaterial("silicon", 1.5F))};
    scene.AddSolid(MakeBox("world", 1, airID, {10.0F, 10.0F, 10.0F}, 0));
    auto sensor{MakeBox("sensor", 2, siliconID, {0.5F, 2.0F, 2.0F}, 1)};
    sensor.fTransform.fTranslationMm = {2.5F, 0.0F, 0.0F};
    sensor.fSensorID = 7;
    scene.AddSolid(sensor);
    scene.SetWorldVolumeID(1);

    Surface surface{};
    surface.fName = "sensor";
    surface.fKind = SurfaceKind::DielectricMetal;
    surface.fReflectivity.fEnergyEv = {1.0F, 10.0F};
    surface.fReflectivity.fValues = {0.0F, 0.0F};
    surface.fEfficiency.fEnergyEv = {1.0F, 10.0F};
    surface.fEfficiency.fValues = {1.0F, 1.0F};
    const auto surfaceID{scene.AddSurface(std::move(surface))};
    scene.AddSurfaceBinding({1, 2, surfaceID});

    Photon photon{};
    photon.fPositionMm = {0.0F, 0.0F, 0.0F};
    photon.fDirection = {1.0F, 0.0F, 0.0F};
    photon.fEnergyEv = 3.0F;
    photon.fVolumeID = 1;

    const auto result{CpuOpticalTransport{TransportConfig{}}.Transport(
        scene, {&photon, 1})};
    return Expect(result.fStats.fDetectedCount == 1 &&
                      result.fHitData.size() == 1 &&
                      result.fHitData.front().fSensorID == 7,
                  "sensor surface should produce one sensor hit");
}

} // namespace

auto main() -> int {
    const auto geometry{TestGeometry()};
    const auto absorption{TestAbsorption()};
    const auto sensor{TestSensor()};
    if (geometry && absorption && sensor) {
        std::cout << "Optical core tests passed\n";
        return 0;
    }
    return 1;
}
