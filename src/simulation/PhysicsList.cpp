#include "g4go/simulation/PhysicsList.hpp"

#include "G4EmStandardPhysics_option3.hh"
#include "G4Exception.hh"
#include "G4LossTableManager.hh"
#include "G4OpticalParameters.hh"
#include "G4OpticalPhoton.hh"
#include "G4OpticalPhysics.hh"
#include "G4ProcessManager.hh"
#include "G4ProcessVector.hh"
#include "G4QuasiCerenkov.hh"
#include "G4QuasiOpticalPhoton.hh"
#include "G4RadioactiveDecayPhysics.hh"
#include "G4VProcess.hh"
#include "g4go/optical/geant4/G4GOQuasiScintillation.hpp"

namespace G4GO::Simulation {

PhysicsList::PhysicsList(G4bool useOpticalOffload) :
    FTFP_BERT{0},
    fUseOpticalOffload{useOpticalOffload} {
    RegisterPhysics(new G4OpticalPhysics{0});
    RegisterPhysics(new G4RadioactiveDecayPhysics{0});
    ReplacePhysics(new G4EmStandardPhysics_option3{0});
}

auto PhysicsList::ConstructParticle() -> void {
    FTFP_BERT::ConstructParticle();
    if (fUseOpticalOffload) {
        G4QuasiOpticalPhoton::QuasiOpticalPhotonDefinition();
    }
}

auto PhysicsList::ConstructProcess() -> void {
    FTFP_BERT::ConstructProcess();

    if (fUseOpticalOffload) {
        auto* parameters{G4OpticalParameters::Instance()};

        G4QuasiCerenkov* cerenkov{nullptr};
        if (parameters->GetProcessActivation("QuasiCerenkov") or parameters->GetCerenkovOffloadPhotons()) {
            cerenkov = new G4QuasiCerenkov{"QuasiCerenkov"};
            cerenkov->SetMaxNumPhotonsPerStep(parameters->GetCerenkovMaxPhotonsPerStep());
            cerenkov->SetMaxBetaChangePerStep(parameters->GetCerenkovMaxBetaChange());
            cerenkov->SetTrackSecondariesFirst(parameters->GetCerenkovTrackSecondariesFirst());
            cerenkov->SetStackPhotons(true);
            cerenkov->SetVerboseLevel(parameters->GetCerenkovVerboseLevel());
        }

        G4GOQuasiScintillation* scintillation{nullptr};
        if (parameters->GetProcessActivation("QuasiScintillation") or parameters->GetScintOffloadPhotons()) {
            scintillation = new G4GOQuasiScintillation{"QuasiScintillation"};
            scintillation->AddSaturation(G4LossTableManager::Instance()->EmSaturation());
            scintillation->SetScintillationByParticleType(parameters->GetScintByParticleType());
            scintillation->SetScintillationTrackInfo(parameters->GetScintTrackInfo());
            scintillation->SetTrackSecondariesFirst(parameters->GetScintTrackSecondariesFirst());
            scintillation->SetFiniteRiseTime(parameters->GetScintFiniteRiseTime());
            scintillation->SetStackPhotons(true);
            scintillation->SetOffloadPhotons(true);
            scintillation->SetVerboseLevel(parameters->GetScintVerboseLevel());
        }

        auto* iterator{GetParticleIterator()};
        iterator->reset();
        while ((*iterator)()) {
            auto* particle{iterator->value()};
            if (particle->IsShortLived()) {
                continue;
            }
            auto* processManager{particle->GetProcessManager()};
            if (processManager == nullptr) {
                G4ExceptionDescription description{};
                description << "Particle " << particle->GetParticleName() << " has no process manager";
                G4Exception("G4GOPhysicsList", "G4GOProcessManager", FatalException, description);
                return;
            }
            if (cerenkov != nullptr and cerenkov->IsApplicable(*particle)) {
                processManager->AddDiscreteProcess(cerenkov);
            }
            if (scintillation != nullptr and scintillation->IsApplicable(*particle)) {
                processManager->AddProcess(scintillation);
                processManager->SetProcessOrderingToLast(scintillation, idxAtRest);
                processManager->SetProcessOrderingToLast(scintillation, idxPostStep);
            }
        }
    }

    auto* processManager{G4OpticalPhoton::OpticalPhotonDefinition()->GetProcessManager()};
    if (processManager == nullptr) {
        return;
    }
    auto* processes{processManager->GetProcessList()};
    for (auto index{processManager->GetProcessListLength() - 1}; index >= 0; --index) {
        auto* process{(*processes)[index]};
        if (process != nullptr and process->GetProcessName() == "Scintillation") {
            processManager->RemoveProcess(process);
        }
    }
}

} // namespace G4GO::Simulation
