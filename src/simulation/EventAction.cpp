#include "g4go/simulation/EventAction.hpp"

#include "G4Event.hh"
#include "G4RunManager.hh"
#include "G4SDManager.hh"
#include "G4SystemOfUnits.hh"
#include "g4go/detector/DetectorConstruction.hpp"
#include "g4go/detector/ScintillatorHit.hpp"
#include "g4go/detector/SensorHit.hpp"
#include "g4go/optical/geant4/Geant4EventAdapter.hpp"
#include "g4go/simulation/Analysis.hpp"

#include <cstddef>
#include <utility>

namespace G4GO::Simulation {

EventAction::EventAction(
    std::shared_ptr<G4GO::Optical::Geant4EventAdapter> adapter,
    std::shared_ptr<Analysis> analysis) :
    fAdapter{std::move(adapter)},
    fAnalysis{std::move(analysis)} {}

auto EventAction::BeginOfEventAction(const G4Event* event) -> void {
    fAdapter->BeginEvent(event->GetEventID());
}

auto EventAction::EndOfEventAction(const G4Event* event) -> void {
    const auto transportFuture{fAdapter->EndEvent()};

    auto scintillatorHCid{
        G4SDManager::GetSDMpointer()->GetCollectionID(
            "ScintillatorHitsCollection")};
    auto scintHC{static_cast<G4GO::Detector::ScintillatorHC*>(
        event->GetHCofThisEvent()->GetHC(scintillatorHCid))};

    auto sensorHCid{
        G4SDManager::GetSDMpointer()->GetCollectionID("SensorHitsCollection")};
    auto sensorHC{static_cast<G4GO::Detector::SensorHC*>(
        event->GetHCofThisEvent()->GetHC(sensorHCid))};

    const auto moduleID{static_cast<const G4GO::Detector::DetectorConstruction*>(
                            G4RunManager::GetRunManager()->GetUserDetectorConstruction())
                            ->ModuleID()};
    const auto eventID{event->GetEventID()};
    std::vector<CrystalHitOutput> crystalHits{};
    crystalHits.reserve(moduleID);
    for (auto i{0}; i < moduleID; ++i) {
        const auto energyDeposit{
            scintHC->GetVector()->at(static_cast<std::size_t>(i))->GetEnergyDeposit()};
        if (energyDeposit > 0.) {
            crystalHits.emplace_back(
                CrystalHitOutput{eventID, i, energyDeposit});
        }
    }

    std::vector<SensorHitOutput> sensorHits{};
    if (!transportFuture.valid()) {
        sensorHits.reserve(sensorHC->entries());
        for (auto index{std::size_t{}}; index < sensorHC->entries(); ++index) {
            const auto* sensorHit{sensorHC->GetVector()->at(index)};
            sensorHits.emplace_back(SensorHitOutput{
                eventID,
                sensorHit->GetCopyNo(),
                static_cast<double>(sensorHit->GetGlobalTime() / ns),
            });
        }
    }

    fAnalysis->Enqueue(std::move(crystalHits), std::move(sensorHits),
                       transportFuture);
    if (fAnalysis->PendingCount() >=
        fAdapter->Configuration().fMaxPendingEvents) {
        fAnalysis->WaitAndWriteNextEvent();
    } else {
        fAnalysis->WriteReadyEvents();
    }
}

} // namespace G4GO::Simulation
