#include "g4go/detector/DetectorConstruction.hpp"

#include "G4Box.hh"
#include "G4LogicalBorderSurface.hh"
#include "G4LogicalSkinSurface.hh"
#include "G4LogicalVolume.hh"
#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4NistManager.hh"
#include "G4OpticalSurface.hh"
#include "G4PVPlacement.hh"
#include "G4SDManager.hh"
#include "G4SystemOfUnits.hh"
#include "G4ThreeVector.hh"
#include "g4go/detector/ScintillatorSD.hpp"
#include "g4go/detector/SensorSD.hpp"

#include <vector>

namespace G4GO::Detector {

DetectorConstruction::DetectorConstruction() :
    fCheckOverlap{false} {}

auto DetectorConstruction::Construct() -> G4VPhysicalVolume* {
    const auto nist{G4NistManager::Instance()};

    const auto hydrogenElement{nist->FindOrBuildElement("H")};
    const auto carbonElement{nist->FindOrBuildElement("C")};
    const auto oxygenElement{nist->FindOrBuildElement("O")};
    const auto siliconElement{nist->FindOrBuildElement("Si")};

    const auto cesiumIodide{nist->FindOrBuildMaterial("G4_CESIUM_IODIDE")};
    const auto air{nist->BuildMaterialWithNewDensity("Vacuum", "G4_AIR", 1e-12 * g / cm3)};
    const auto silicon{nist->FindOrBuildMaterial("G4_Si")};

    const auto siliconeGrease{new G4Material("siliconeGrease", 1.06 * g / cm3, 4, kStateLiquid)};
    siliconeGrease->AddElement(carbonElement, 2);
    siliconeGrease->AddElement(hydrogenElement, 6);
    siliconeGrease->AddElement(oxygenElement, 1);
    siliconeGrease->AddElement(siliconElement, 1);

    const auto epoxy{new G4Material("epoxy", 1.18 * g / cm3, 3, kStateSolid)};
    epoxy->AddElement(carbonElement, 0.7362);
    epoxy->AddElement(hydrogenElement, 0.0675);
    epoxy->AddElement(oxygenElement, 0.1963);

    const std::vector<double> scintillatorEnergyBin{
        3.300891 * eV, 3.394291 * eV, 3.459551 * eV, 3.515883 * eV, 3.557591 * eV, 3.591915 * eV,
        3.622042 * eV, 3.644458 * eV, 3.678815 * eV, 3.690132 * eV, 3.715531 * eV, 3.728362 * eV,
        3.747776 * eV, 3.768708 * eV, 3.787216 * eV, 3.80725 * eV, 3.820723 * eV, 3.847958 * eV,
        3.871416 * eV, 3.9022 * eV, 3.932045 * eV, 4.042466 * eV, 4.157667 * eV, 4.193192 * eV,
        4.224366 * eV, 4.24096 * eV, 4.257685 * eV, 4.274542 * eV, 4.291534 * eV, 4.306942 * eV,
        4.343328 * eV, 4.359111 * eV, 4.396388 * eV, 4.437953 * eV, 4.471035 * eV, 4.511202 * eV,
        4.546346 * eV, 4.602552 * eV, 4.662178 * eV, 4.733725 * eV, 4.883613 * eV, 5.114986 * eV};
    const std::vector<double> scintillationComponent1{
        0.152652, 0.190301, 0.233538, 0.282997, 0.328661, 0.379536,
        0.430119, 0.47672, 0.531663, 0.57447, 0.617499, 0.656175,
        0.695466, 0.729048, 0.768509, 0.803861, 0.841795, 0.88913,
        0.937399, 0.977291, 1.0, 0.972946, 0.880537, 0.821459,
        0.754517, 0.691979, 0.640116, 0.591668, 0.540922, 0.501151,
        0.452269, 0.413595, 0.35813, 0.307095, 0.264469, 0.218419,
        0.182053, 0.129528, 0.081253, 0.047185, 0.017148, 0.0};
    const std::vector<double> sensorEnergyBin{
        1.771068 * eV, 2.101763 * eV, 2.478823 * eV, 2.66882 * eV, 2.756137 * eV, 2.849362 * eV,
        2.949113 * eV, 3.02425 * eV, 3.099632 * eV, 3.178868 * eV, 3.3981 * eV, 3.649811 * eV,
        3.877416 * eV, 4.128759 * eV, 4.348776 * eV};
    const std::vector<double> sensorEfficiency{
        0.26319, 0.395706, 0.503681, 0.525767, 0.555215, 0.570552,
        0.597546, 0.59816, 0.599387, 0.593252, 0.521472, 0.466258,
        0.458896, 0.408589, 0.196933};

    const auto [minPhotonEnergy, maxPhotonEnergy]{std::ranges::minmax(scintillatorEnergyBin)};

    const auto crystalPropertiesTable{new G4MaterialPropertiesTable};
    crystalPropertiesTable->AddProperty("ABSLENGTH", {minPhotonEnergy, maxPhotonEnergy}, {40 * cm, 40 * cm});
    crystalPropertiesTable->AddProperty("SCINTILLATIONCOMPONENT1", scintillatorEnergyBin, scintillationComponent1);
    crystalPropertiesTable->AddConstProperty("SCINTILLATIONYIELD", 3500. / MeV);
    crystalPropertiesTable->AddConstProperty("SCINTILLATIONTIMECONSTANT1", 30 * ns);
    crystalPropertiesTable->AddConstProperty("RESOLUTIONSCALE", 1.0);
    crystalPropertiesTable->AddProperty("RINDEX", {minPhotonEnergy, maxPhotonEnergy}, {1.95, 1.95});
    cesiumIodide->SetMaterialPropertiesTable(crystalPropertiesTable);

    const auto airPropertiesTable{new G4MaterialPropertiesTable()};
    airPropertiesTable->AddProperty("RINDEX", {minPhotonEnergy, maxPhotonEnergy}, {1., 1.});
    air->SetMaterialPropertiesTable(airPropertiesTable);

    const auto siliconeGreasePropertiesTable{new G4MaterialPropertiesTable};
    siliconeGreasePropertiesTable->AddProperty("RINDEX", {minPhotonEnergy, maxPhotonEnergy}, {1.46, 1.46});
    siliconeGreasePropertiesTable->AddProperty("ABSLENGTH", {minPhotonEnergy, maxPhotonEnergy}, {100 * cm, 100 * cm});
    siliconeGrease->SetMaterialPropertiesTable(siliconeGreasePropertiesTable);

    const auto epoxyPropertiesTable{new G4MaterialPropertiesTable};
    epoxyPropertiesTable->AddProperty("RINDEX", {minPhotonEnergy, maxPhotonEnergy}, {1.57, 1.57});
    epoxy->SetMaterialPropertiesTable(epoxyPropertiesTable);

    const auto crystalWidth{3 * cm};
    const auto crystalLength{8 * cm};

    const auto transform =
        [crystalTail = G4ThreeVector(0, 0, crystalLength / 2)](double transformDistance) {
            return G4Translate3D{crystalTail + G4ThreeVector(0, 0, transformDistance)};
        };

    const auto worldSize{2 * m};
    const auto solidWorld{new G4Box("world", 0.5 * worldSize, 0.5 * worldSize, 0.5 * worldSize)};
    const auto logicalWorld{new G4LogicalVolume(solidWorld, air, "World")};
    const auto physicalWorld{
        new G4PVPlacement(
            G4Transform3D{},
            logicalWorld,
            "World",
            nullptr,
            false,
            0,
            fCheckOverlap)};

    const auto coupleThickness{0.1 * mm};
    const auto windowThickness{1 * mm};
    const auto sipmWidth{3 * mm};
    const auto sipmThickness{0.1 * mm};
    const auto sipmArraySize{8};
    const auto sipmPitch{sipmWidth + 0.2 * mm};
    const auto sipmArrayOffset{(sipmArraySize - 1) * sipmPitch / 2};

    const auto solidCoupler{new G4Box("Coupler", sipmWidth / 2, sipmWidth / 2, coupleThickness / 2)};
    const auto logicalCoupler{new G4LogicalVolume(solidCoupler, siliconeGrease, "Coupler")};
    new G4PVPlacement(
        transform(coupleThickness / 2),
        logicalCoupler,
        "Coupler",
        logicalWorld,
        false,
        0,
        fCheckOverlap);

    const auto solidWindow{
        new G4Box(
            "Window",
            sipmWidth / 2,
            sipmWidth / 2,
            windowThickness / 2)};
    const auto logicalWindow{new G4LogicalVolume(solidWindow, epoxy, "Window")};
    new G4PVPlacement(
        transform(coupleThickness + windowThickness / 2),
        logicalWindow,
        "Window",
        logicalWorld,
        false,
        0,
        fCheckOverlap);

    const auto solidCrystal{new G4Box("Crystal", crystalWidth / 2, crystalWidth / 2, crystalLength / 2)};
    const auto logicalCrystal{new G4LogicalVolume(solidCrystal, cesiumIodide, "Crystal")};
    const auto physicalCrystal{new G4PVPlacement(G4Transform3D{}, logicalCrystal, "Crystal", logicalWorld, false, 0, fCheckOverlap)};

    const auto solidSiPM{new G4Box("SiPM", sipmWidth / 2, sipmWidth / 2, sipmThickness / 2)};
    const auto logicalSiPM{new G4LogicalVolume(solidSiPM, silicon, "SiPM")};
    const auto sipmPositionZ{crystalLength / 2 + coupleThickness + windowThickness + sipmThickness / 2};
    for (auto i{0}; i < sipmArraySize; ++i) {
        for (auto j{0}; j < sipmArraySize; ++j) {
            const auto sipmPositionX{sipmArrayOffset - i * sipmPitch};
            const auto sipmPositionY{sipmArrayOffset - j * sipmPitch};
            new G4PVPlacement(
                G4Translate3D{
                    G4ThreeVector{sipmPositionX, sipmPositionY, sipmPositionZ}
            },
                logicalSiPM,
                "SiPM",
                logicalWorld,
                false,
                i * sipmArraySize + j,
                fCheckOverlap);
        }
    }

    const auto reflectorSurfacePropertiesTable{new G4MaterialPropertiesTable};
    reflectorSurfacePropertiesTable->AddProperty("REFLECTIVITY", {minPhotonEnergy, maxPhotonEnergy}, {0.99, 0.99});

    const auto reflectorSurface{new G4OpticalSurface("Reflector", unified, polished, dielectric_metal)};
    new G4LogicalBorderSurface("ReflectorSurface", physicalCrystal, physicalWorld, reflectorSurface);
    reflectorSurface->SetMaterialPropertiesTable(reflectorSurfacePropertiesTable);

    const auto coatingSurfacePropertiesTable{new G4MaterialPropertiesTable};
    coatingSurfacePropertiesTable->AddProperty("REFLECTIVITY", {minPhotonEnergy, maxPhotonEnergy}, {0., 0.});

    const auto coatingSurface{new G4OpticalSurface("Coating", unified, polished, dielectric_metal)};
    new G4LogicalBorderSurface("CoatingSurface", physicalWorld, physicalCrystal, coatingSurface);
    coatingSurface->SetMaterialPropertiesTable(coatingSurfacePropertiesTable);

    const auto cathodeSurfacePropertiesTable{new G4MaterialPropertiesTable};
    cathodeSurfacePropertiesTable->AddProperty("REFLECTIVITY", {minPhotonEnergy, maxPhotonEnergy}, {0., 0.});
    cathodeSurfacePropertiesTable->AddProperty("EFFICIENCY", sensorEnergyBin, sensorEfficiency);

    const auto cathodeSurface{new G4OpticalSurface("Cathode", unified, polished, dielectric_metal)};
    cathodeSurface->SetMaterialPropertiesTable(cathodeSurfacePropertiesTable);
    new G4LogicalSkinSurface("cathodeSkinSurface", logicalSiPM, cathodeSurface);

    ++moduleID;

    return physicalWorld;
}

auto DetectorConstruction::ConstructSDandField() -> void {
    auto scintillatorSD{
        new ScintillatorSD("ScintillatorSD", "ScintillatorHitsCollection")};
    G4SDManager::GetSDMpointer()->AddNewDetector(scintillatorSD);
    SetSensitiveDetector("Crystal", scintillatorSD, true);

    auto sensorSD{
        new SensorSD("SensorSD", "SensorHitsCollection")};
    G4SDManager::GetSDMpointer()->AddNewDetector(sensorSD);
    SetSensitiveDetector("SiPM", sensorSD, true);
}

} // namespace G4GO::Detector
