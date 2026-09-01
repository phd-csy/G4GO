#include "CLI/CLI.hpp"
#include "FTFP_BERT.hh"
#include "G4EmStandardPhysics_option3.hh"
#include "G4OpticalParameters.hh"
#include "G4OpticalPhysics.hh"
#include "G4RadioactiveDecayPhysics.hh"
#include "G4RunManagerFactory.hh"
#include "G4SteppingVerbose.hh"
#include "G4Threading.hh"
#include "G4Timer.hh"
#include "g4go/detector/DetectorConstruction.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"
#include "g4go/simulation/ActionInitialization.hpp"
#ifdef G4GO_USE_UIVIS
#    include "G4UIExecutive.hh"
#endif
#include "G4UImanager.hh"
#include "G4VModularPhysicsList.hh"
#ifdef G4GO_USE_UIVIS
#    include "G4VisExecutive.hh"
#endif
#include "G4ios.hh"
#include "Randomize.hh"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

struct CommandLineOptions {
    G4GO::Optical::PhotonTransportConfig photonTransportConfig{};
    std::optional<std::string> macroFile{};
    std::uint32_t threads{};
    bool help{};
};

} // namespace

auto main(int argc, char** argv) -> int {
    CLI::App app{"G4GO optical photon simulation", argv[0]};
    CommandLineOptions options{};
    std::string backendName{
        G4GO::Optical::ToString(
            options.photonTransportConfig.fBackend)};
    std::string macroFile{};

    app.set_help_flag("");
    app.add_flag("-h,--help", options.help, "Print this help message");
    app.add_option(
           "--backend", backendName, "Optical photon transport backend")
        ->check(CLI::IsMember({"auto", "cpu", "gpu"}))
        ->capture_default_str();
    app.add_option(
           "--seed", options.photonTransportConfig.fSeed, "Random seed")
        ->capture_default_str();
    app.add_option(
           "--threads", options.threads, "Number of Geant4 worker threads")
        ->capture_default_str();
    app.add_option(
           "--max-photons",
           options.photonTransportConfig.fMaxPhotonsPerEvent,
           "Maximum number of captured optical photons")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option(
           "--max-bounces",
           options.photonTransportConfig.fMaxBouncesPerPhoton,
           "Maximum number of optical photon bounces")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option(
           "--batch-photons",
           options.photonTransportConfig.fTargetPhotonsPerBatch,
           "Target optical photons per GPU batch")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option(
           "--mesh-rotation-steps",
           options.photonTransportConfig.fMeshRotationSteps,
           "Number of Geant4 polyhedron rotation steps")
        ->check(CLI::Range(8U, 4096U))
        ->capture_default_str();
    app.add_option("macro", macroFile, "Geant4 macro file")
        ->expected(0, 1);

    try {
        app.parse(argc, argv);

        if (backendName == "auto") {
            options.photonTransportConfig.fBackend =
                G4GO::Optical::PhotonTransportBackend::Auto;
        } else if (backendName == "cpu") {
            options.photonTransportConfig.fBackend =
                G4GO::Optical::PhotonTransportBackend::Geant4;
        } else if (backendName == "gpu") {
            options.photonTransportConfig.fBackend =
                G4GO::Optical::PhotonTransportBackend::OptiX;
        } else {
            throw std::invalid_argument(
                "invalid value for --backend: " + backendName);
        }

        if (!macroFile.empty()) {
            options.macroFile = std::move(macroFile);
        }

    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    } catch (const std::exception& exception) {
        G4cerr << "[g4go] " << exception.what() << G4endl;
        G4cout << app.help() << G4endl;
        return 1;
    }

    if (options.help) {
        G4cout << app.help() << G4endl;
        return 0;
    }

    const auto timer{new G4Timer()};
    timer->Start();

#ifdef G4GO_USE_UIVIS
    G4UIExecutive* ui{nullptr};
    if (!options.macroFile) {
        G4int uiArgc{1};
        char* uiArgv[]{argv[0], nullptr};
        ui = new G4UIExecutive(uiArgc, uiArgv);
    }
#else
    if (!options.macroFile) {
        G4cout << app.help() << G4endl;
        delete timer;
        return 1;
    }
#endif

    G4Random::setTheEngine(new CLHEP::MTwistEngine());
    G4Random::setTheSeed(
        static_cast<long>(options.photonTransportConfig.fSeed));

    G4int precision{4};
    G4SteppingVerbose::UseBestUnit(precision);

    const auto runManagerType{G4RunManagerType::Default};
    auto* runManager{G4RunManagerFactory::CreateRunManager(runManagerType)};

#ifdef G4MULTITHREADED
    {
        G4int nThreads = 0;
        nThreads = G4Threading::G4GetNumberOfCores();
        if (options.threads > 0) {
            nThreads = static_cast<G4int>(options.threads);
        }
        runManager->SetNumberOfThreads(nThreads);
    }
#endif

    runManager->SetUserInitialization(
        new G4GO::Detector::DetectorConstruction());

    G4VModularPhysicsList* physicsList{new FTFP_BERT(0)};
    physicsList->RegisterPhysics(new G4OpticalPhysics(0));
    G4OpticalParameters::Instance()->SetBoundaryInvokeSD(true);
    physicsList->RegisterPhysics(new G4RadioactiveDecayPhysics(0));
    physicsList->ReplacePhysics(new G4EmStandardPhysics_option3(0));
    runManager->SetUserInitialization(physicsList);

    runManager->SetUserInitialization(
        new G4GO::Simulation::ActionInitialization(
            options.photonTransportConfig));

#ifdef G4GO_USE_UIVIS
    G4VisManager* visManager{new G4VisExecutive("Quiet")};
    visManager->Initialize();
#endif

    G4UImanager* uiManager{G4UImanager::GetUIpointer()};

#ifdef G4GO_USE_UIVIS
    if (!ui) {
        G4String command{"/control/execute "};
        G4String fileName{*options.macroFile};
        uiManager->ApplyCommand(command + fileName);
    } else {
        uiManager->ApplyCommand("/control/execute scripts/vis.mac");
        ui->SessionStart();
        delete ui;
    }
#else
    G4String command{"/control/execute "};
    G4String fileName{*options.macroFile};
    uiManager->ApplyCommand(command + fileName);
#endif

    timer->Stop();
    G4cout << "Time runs: " << *timer << G4endl;

#ifdef G4GO_USE_UIVIS
    delete visManager;
#endif
    delete runManager;
    delete timer;

    return 0;
}
