#include "g4go/simulation/Analysis.hpp"

#include "G4AnalysisManager.hh"

#include <chrono>
#include <future>
#include <utility>

namespace G4GO::Simulation {

auto Analysis::Enqueue(std::vector<CrystalHitOutput> crystalHits, std::vector<SensorHitOutput> sensorHits,
                       G4GO::Optical::PhotonTransportFuture transportFuture) -> void {
    fPendingEvents.emplace_back(
        PendingEvent{std::move(crystalHits), std::move(sensorHits), std::move(transportFuture)});
}

auto Analysis::WriteReadyEvents() -> void { ProcessReadyEvents(false); }

auto Analysis::WaitAndWriteNextEvent() -> void { ProcessReadyEvents(true); }

auto Analysis::ProcessReadyEvents(bool waitForFrontEvent) -> void {
    if (fPendingEvents.empty()) {
        return;
    }
    if (waitForFrontEvent && fPendingEvents.at(0).transportFuture.valid()) {
        fPendingEvents.at(0).transportFuture.wait();
    }
    while (!fPendingEvents.empty()) {
        auto& event{fPendingEvents.at(0)};
        if (event.transportFuture.valid() &&
            event.transportFuture.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            break;
        }
        auto completedEvent{std::move(event)};
        fPendingEvents.pop_front();
        WriteEvent(std::move(completedEvent));
    }
}

auto Analysis::Flush() -> void {
    while (!fPendingEvents.empty()) {
        WaitAndWriteNextEvent();
    }
}

auto Analysis::WriteEvent(PendingEvent event) -> void {
    const auto hasTransportOutput{event.transportFuture.valid()};
    G4GO::Optical::PhotonTransportOutput output{};
    if (hasTransportOutput) {
        output = event.transportFuture.get();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    const auto detectedCount{hasTransportOutput ? output.statistics.detectedCount : event.sensorHits.size()};
    for (const auto& crystalHit : event.crystalHits) {
        analysisManager->FillNtupleIColumn(0, 0, crystalHit.eventID);
        analysisManager->FillNtupleIColumn(0, 1, crystalHit.moduleID);
        analysisManager->FillNtupleDColumn(0, 2, crystalHit.energyDeposit);
        analysisManager->FillNtupleIColumn(0, 3, static_cast<int>(detectedCount));
        analysisManager->FillNtupleIColumn(0, 4, crystalHit.generatedPhotonCount);
        analysisManager->AddNtupleRow(0);
    }

    if (hasTransportOutput) {
        for (const auto& detection : output.detections) {
            analysisManager->FillNtupleIColumn(1, 0, static_cast<int>(detection.eventID));
            analysisManager->FillNtupleIColumn(1, 1, static_cast<int>(detection.sensorID));
            analysisManager->FillNtupleDColumn(1, 2, detection.timeNs);
            analysisManager->AddNtupleRow(1);
        }
        return;
    }

    for (const auto& sensorHit : event.sensorHits) {
        analysisManager->FillNtupleIColumn(1, 0, sensorHit.eventID);
        analysisManager->FillNtupleIColumn(1, 1, sensorHit.sensorID);
        analysisManager->FillNtupleDColumn(1, 2, sensorHit.timeOfFlight);
        analysisManager->AddNtupleRow(1);
    }
}

} // namespace G4GO::Simulation
