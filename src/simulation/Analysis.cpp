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
        const auto waitStart{std::chrono::steady_clock::now()};
        fPendingEvents.at(0).transportFuture.wait();
        fPerformance.futureWaitMs +=
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - waitStart}.count();
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
    const auto* transportOutput{event.transportFuture.valid() ? &event.transportFuture.get() : nullptr};

    auto analysisManager{G4AnalysisManager::Instance()};
    const auto detectedCount{
        transportOutput != nullptr ? transportOutput->statistics.detectedCount : event.sensorHitOutput.size()};
    for (const auto& crystalHit : event.crystalHitOutput) {
        analysisManager->FillNtupleIColumn(0, 0, crystalHit.eventID);
        analysisManager->FillNtupleIColumn(0, 1, crystalHit.moduleID);
        analysisManager->FillNtupleDColumn(0, 2, crystalHit.energyDeposit);
        analysisManager->FillNtupleIColumn(0, 3, static_cast<int>(detectedCount));
        analysisManager->FillNtupleIColumn(0, 4, crystalHit.generatedPhotonCount);
        analysisManager->AddNtupleRow(0);
    }

    const auto writeSensorHits{[&](const auto& sensorHits, const auto& timeOfFlight) {
        const auto packStart{std::chrono::steady_clock::now()};
        fSensorIDs.clear();
        fSensorTimes.clear();
        if (sensorHits.empty()) {
            fPerformance.sensorPackMs +=
                std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - packStart}.count();
            return;
        }

        fSensorIDs.reserve(sensorHits.size());
        fSensorTimes.reserve(sensorHits.size());
        for (const auto& sensorHit : sensorHits) {
            fSensorIDs.emplace_back(static_cast<int>(sensorHit.sensorID));
            fSensorTimes.emplace_back(static_cast<float>(timeOfFlight(sensorHit)));
        }
        fPerformance.sensorPackMs +=
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - packStart}.count();

        const auto sensorNtupleStart{std::chrono::steady_clock::now()};
        analysisManager->FillNtupleIColumn(1, 0, static_cast<int>(sensorHits.front().eventID));
        analysisManager->AddNtupleRow(1);
        fPerformance.sensorNtupleMs +=
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - sensorNtupleStart}.count();
    }};

    if (transportOutput != nullptr) {
        writeSensorHits(transportOutput->detections, [](const auto& detection) { return detection.timeNs; });
    } else {
        writeSensorHits(event.sensorHitOutput, [](const auto& sensorHit) { return sensorHit.timeOfFlight; });
    }
}

} // namespace G4GO::Simulation
