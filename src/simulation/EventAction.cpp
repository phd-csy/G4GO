#include "g4go/simulation/EventAction.hpp"

#include "G4Event.hh"
#include "G4HCofThisEvent.hh"
#include "G4SDManager.hh"
#include "G4SystemOfUnits.hh"
#include "g4go/detector/ScintillatorHit.hpp"
#include "g4go/detector/SensorHit.hpp"
#include "g4go/optical/geant4/Geant4EventAdapter.hpp"
#include "g4go/simulation/Analysis.hpp"

#include <cstddef>
#include <utility>

namespace G4GO::Simulation {

EventAction::EventAction(std::shared_ptr<G4GO::Optical::Geant4EventAdapter> adapter,
                         std::shared_ptr<Analysis> analysis) :
    fAdapter{std::move(adapter)},
    fAnalysis{std::move(analysis)} {}

auto EventAction::BeginOfEventAction(const G4Event* event) -> void { fAdapter->BeginEvent(event->GetEventID()); }

auto EventAction::EndOfEventAction(const G4Event* event) -> void {
    const auto transportFuture{fAdapter->EndEvent()};

    auto* collections{event->GetHCofThisEvent()};
    const auto collection{[collections](const char* name) -> G4VHitsCollection* {
        if (collections == nullptr) {
            return nullptr;
        }
        const auto id{G4SDManager::GetSDMpointer()->GetCollectionID(name)};
        return id >= 0 and static_cast<std::size_t>(id) < collections->GetCapacity() ? collections->GetHC(id) : nullptr;
    }};
    const auto* scintillatorHC{static_cast<G4GO::Detector::ScintillatorHC*>(collection("ScintillatorHitsCollection"))};
    const auto* sensorHC{static_cast<G4GO::Detector::SensorHC*>(collection("SensorHitsCollection"))};
    const auto eventID{event->GetEventID()};
    const auto generatedPhotonCount{static_cast<int>(fAdapter->EventStatistics().generatedCount)};
    std::vector<ScintillatorHitOutput> scintillatorHits{};
    const auto moduleCount{scintillatorHC ? scintillatorHC->entries() : 0U};
    scintillatorHits.reserve(moduleCount);
    for (std::size_t i{}; i < moduleCount; ++i) {
        const auto energyDeposit{(*scintillatorHC)[i]->EnergyDeposit()};
        if (energyDeposit > 0.) {
            scintillatorHits.emplace_back(ScintillatorHitOutput{
                eventID, static_cast<int>(i), static_cast<float>(energyDeposit), generatedPhotonCount});
        }
    }

    std::vector<SensorHitOutput> sensorHits{};
    if (sensorHC != nullptr and not transportFuture.valid()) {
        sensorHits.reserve(sensorHC->entries());
        for (std::size_t i{}; i < sensorHC->entries(); i++) {
            const auto* sensorHit{sensorHC->GetVector()->at(i)};
            sensorHits.emplace_back(SensorHitOutput{
                eventID,
                sensorHit->CopyNo(),
                static_cast<float>(sensorHit->GlobalTime() / ns),
            });
        }
    }

    fAnalysis->Enqueue(std::move(scintillatorHits), std::move(sensorHits), transportFuture);
    if (fAnalysis->PendingCount() >= fAdapter->Configuration().maxPendingEvents) {
        fAnalysis->WaitAndWriteNextEvent();
    } else {
        fAnalysis->WriteReadyEvents();
    }
}

} // namespace G4GO::Simulation
