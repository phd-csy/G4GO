#include "g4go/simulation/PhysicsList.hpp"

#include "G4Exception.hh"
#include "G4LossTableManager.hh"
#include "G4OpticalParameters.hh"
#include "G4ProcessManager.hh"
#include "G4QuasiCerenkov.hh"
#include "G4QuasiOpticalPhoton.hh"
#include "G4QuasiScintillation.hh"
#include "G4VModularPhysicsList.hh"
#include "G4VPhysicsConstructor.hh"

namespace G4GO::Simulation {

namespace {

class QuasiOpticalPhysicsConstructor final : public G4VPhysicsConstructor {
public:
    explicit QuasiOpticalPhysicsConstructor(G4int verbose) :
        G4VPhysicsConstructor{"G4GOQuasiOptical"} {
        verboseLevel = verbose;
    }

    auto ConstructParticle() -> void override {
        G4QuasiOpticalPhoton::QuasiOpticalPhotonDefinition();
    }

    auto ConstructProcess() -> void override {
        auto* parameters{G4OpticalParameters::Instance()};

        G4QuasiCerenkov* cerenkov{nullptr};
        if (parameters->GetProcessActivation("QuasiCerenkov") ||
            parameters->GetCerenkovOffloadPhotons()) {
            cerenkov = new G4QuasiCerenkov{"QuasiCerenkov"};
            cerenkov->SetMaxNumPhotonsPerStep(
                parameters->GetCerenkovMaxPhotonsPerStep());
            cerenkov->SetMaxBetaChangePerStep(
                parameters->GetCerenkovMaxBetaChange());
            cerenkov->SetTrackSecondariesFirst(
                parameters->GetCerenkovTrackSecondariesFirst());
            cerenkov->SetStackPhotons(parameters->GetCerenkovStackPhotons());
            cerenkov->SetOffloadPhotons(true);
            cerenkov->SetVerboseLevel(
                parameters->GetCerenkovVerboseLevel());
        }

        G4QuasiScintillation* scintillation{nullptr};
        if (parameters->GetProcessActivation("QuasiScintillation") ||
            parameters->GetScintOffloadPhotons()) {
            scintillation = new G4QuasiScintillation{"QuasiScintillation"};
            scintillation->AddSaturation(
                G4LossTableManager::Instance()->EmSaturation());
            scintillation->SetScintillationByParticleType(
                parameters->GetScintByParticleType());
            scintillation->SetScintillationTrackInfo(
                parameters->GetScintTrackInfo());
            scintillation->SetTrackSecondariesFirst(
                parameters->GetScintTrackSecondariesFirst());
            scintillation->SetFiniteRiseTime(
                parameters->GetScintFiniteRiseTime());
            scintillation->SetStackPhotons(parameters->GetScintStackPhotons());
            scintillation->SetOffloadPhotons(true);
            scintillation->SetVerboseLevel(
                parameters->GetScintVerboseLevel());
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
                description << "Particle " << particle->GetParticleName()
                            << " has no process manager";
                G4Exception("RegisterOpticalOffloadProcesses",
                            "G4GOProcessManager", FatalException,
                            description);
                return;
            }
            if (cerenkov != nullptr && cerenkov->IsApplicable(*particle)) {
                processManager->AddDiscreteProcess(cerenkov);
            }
            if (scintillation != nullptr &&
                scintillation->IsApplicable(*particle)) {
                processManager->AddProcess(scintillation);
                processManager->SetProcessOrderingToLast(scintillation,
                                                         idxAtRest);
                processManager->SetProcessOrderingToLast(scintillation,
                                                         idxPostStep);
            }
        }
    }
};

} // namespace

auto RegisterOpticalOffloadProcesses(G4VModularPhysicsList& physicsList)
    -> void {
    physicsList.RegisterPhysics(new QuasiOpticalPhysicsConstructor{0});
}

} // namespace G4GO::Simulation
