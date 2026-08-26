#include "g4go/simulation/Output.hpp"

#include "G4AnalysisManager.hh"

#include <chrono>
#include <future>
#include <utility>

namespace G4GO::Simulation {

auto Output::Enqueue(
    std::vector<CrystalHitRecord> crystalHits,
    G4GO::Optical::PhotonTransportFuture transportResult) -> void {
    fPendingEvents.push_back(
        {std::move(crystalHits), std::move(transportResult)});
}

auto Output::WriteReadyEvents() -> void {
    ProcessReadyEvents(false);
}

auto Output::WaitAndWriteNextEvent() -> void {
    ProcessReadyEvents(true);
}

auto Output::ProcessReadyEvents(bool waitForFrontEvent) -> void {
    if (fPendingEvents.empty()) {
        return;
    }
    if (waitForFrontEvent) {
        fPendingEvents.front().fTransportResult.wait();
    }
    while (!fPendingEvents.empty()) {
        auto& event{fPendingEvents.front()};
        if (event.fTransportResult.valid() &&
            event.fTransportResult.wait_for(std::chrono::seconds{0}) !=
                std::future_status::ready) {
            break;
        }
        auto completedEvent{std::move(event)};
        fPendingEvents.pop_front();
        WriteEvent(std::move(completedEvent));
    }
}

auto Output::Flush() -> void {
    while (!fPendingEvents.empty()) {
        WaitAndWriteNextEvent();
    }
}

auto Output::WriteEvent(PendingEvent event) -> void {
    G4GO::Optical::PhotonTransportOutput result{};
    if (event.fTransportResult.valid()) {
        result = event.fTransportResult.get();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    for (const auto& crystalHit : event.fCrystalHits) {
        analysisManager->FillNtupleIColumn(0, 0, crystalHit.fEventID);
        analysisManager->FillNtupleIColumn(0, 1, crystalHit.fModuleID);
        analysisManager->FillNtupleDColumn(0, 2, crystalHit.fEnergyDeposit);
        analysisManager->FillNtupleIColumn(
            0, 3, static_cast<int>(result.fStatistics.fDetectedCount));
        analysisManager->AddNtupleRow(0);
    }

    for (const auto& detection : result.fDetections) {
        analysisManager->FillNtupleIColumn(
            1, 0, static_cast<int>(detection.fEventID));
        analysisManager->FillNtupleIColumn(
            1, 1, static_cast<int>(detection.fSensorID));
        analysisManager->FillNtupleDColumn(1, 2, detection.fTimeNs);
        analysisManager->AddNtupleRow(1);
    }
}

} // namespace G4GO::Simulation
