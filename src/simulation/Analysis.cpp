#include "g4go/simulation/Analysis.hpp"

#include "G4AnalysisManager.hh"

#include <chrono>
#include <future>
#include <utility>

namespace G4GO::Simulation {

auto Analysis::Enqueue(
    std::vector<CrystalHitOutput> crystalHits,
    std::vector<SensorHitOutput> sensorHits,
    G4GO::Optical::PhotonTransportFuture transportFuture) -> void {
    fPendingEvents.emplace_back(
        PendingEvent{std::move(crystalHits), std::move(sensorHits),
                     std::move(transportFuture)});
}

auto Analysis::WriteReadyEvents() -> void {
    ProcessReadyEvents(false);
}

auto Analysis::WaitAndWriteNextEvent() -> void {
    ProcessReadyEvents(true);
}

auto Analysis::ProcessReadyEvents(bool waitForFrontEvent) -> void {
    if (fPendingEvents.empty()) {
        return;
    }
    if (waitForFrontEvent &&
        fPendingEvents.at(0).fTransportFuture.valid()) {
        fPendingEvents.at(0).fTransportFuture.wait();
    }
    while (!fPendingEvents.empty()) {
        auto& event{fPendingEvents.at(0)};
        if (event.fTransportFuture.valid() &&
            event.fTransportFuture.wait_for(std::chrono::seconds{0}) !=
                std::future_status::ready) {
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
    const auto hasTransportOutput{event.fTransportFuture.valid()};
    G4GO::Optical::PhotonTransportOutput output{};
    if (hasTransportOutput) {
        output = event.fTransportFuture.get();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    const auto detectedCount{hasTransportOutput ?
                                 output.fStatistics.fDetectedCount :
                                 event.fSensorHits.size()};
    for (const auto& crystalHit : event.fCrystalHits) {
        analysisManager->FillNtupleIColumn(0, 0, crystalHit.fEventID);
        analysisManager->FillNtupleIColumn(0, 1, crystalHit.fModuleID);
        analysisManager->FillNtupleDColumn(0, 2, crystalHit.fEnergyDeposit);
        analysisManager->FillNtupleIColumn(
            0, 3, static_cast<int>(detectedCount));
        analysisManager->FillNtupleIColumn(
            0, 4, crystalHit.fGeneratedPhotonCount);
        analysisManager->AddNtupleRow(0);
    }

    if (hasTransportOutput) {
        for (const auto& detection : output.fDetections) {
            analysisManager->FillNtupleIColumn(
                1, 0, static_cast<int>(detection.fEventID));
            analysisManager->FillNtupleIColumn(
                1, 1, static_cast<int>(detection.fSensorID));
            analysisManager->FillNtupleDColumn(1, 2, detection.fTimeNs);
            analysisManager->AddNtupleRow(1);
        }
        return;
    }

    for (const auto& sensorHit : event.fSensorHits) {
        analysisManager->FillNtupleIColumn(1, 0, sensorHit.fEventID);
        analysisManager->FillNtupleIColumn(1, 1, sensorHit.fSensorID);
        analysisManager->FillNtupleDColumn(1, 2, sensorHit.fTimeOfFlight);
        analysisManager->AddNtupleRow(1);
    }
}

} // namespace G4GO::Simulation
