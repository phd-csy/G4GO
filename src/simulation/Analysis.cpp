#include "g4go/simulation/Analysis.hpp"

#include "G4AnalysisManager.hh"

#include <chrono>
#include <future>
#include <utility>

namespace G4GO::Simulation {

auto Analysis::Enqueue(std::vector<CrystalHitOutput> crystalHitOutput, std::vector<SensorHitOutput> sensorHitOutput,
                       G4GO::Optical::PhotonTransportFuture transportFuture) -> void {
    fPendingEvents.emplace_back(
        PendingEvent{std::move(crystalHitOutput), std::move(sensorHitOutput), std::move(transportFuture)});
}

auto Analysis::WriteReadyEvents() -> void { ProcessReadyEvents(false); }

auto Analysis::WaitAndWriteNextEvent() -> void { ProcessReadyEvents(true); }

auto Analysis::ProcessReadyEvents(bool waitForFrontEvent) -> void {
    if (fPendingEvents.empty()) {
        return;
    }
    if (waitForFrontEvent and fPendingEvents.at(0).transportFuture.valid()) {
        fPendingEvents.at(0).transportFuture.wait();
    }
    while (not fPendingEvents.empty()) {
        auto& event{fPendingEvents.at(0)};
        if (event.transportFuture.valid() and
            event.transportFuture.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            break;
        }
        auto completedEvent{std::move(event)};
        fPendingEvents.pop_front();
        WriteEvent(completedEvent);
    }
}

auto Analysis::Flush() -> void {
    while (not fPendingEvents.empty()) {
        WaitAndWriteNextEvent();
    }
}

auto Analysis::WriteEvent(const PendingEvent& event) -> void {
    const auto* transportOutput{event.transportFuture.valid() ? &event.transportFuture.get() : nullptr};

    auto analysisManager{G4AnalysisManager::Instance()};
    const auto detectedCount{transportOutput != nullptr ? transportOutput->statistics.detectedCount :
                                                          event.sensorHitOutput.size()};
    for (const auto& crystalHit : event.crystalHitOutput) {
        analysisManager->FillNtupleIColumn(0, 0, crystalHit.eventID);
        analysisManager->FillNtupleIColumn(0, 1, crystalHit.moduleID);
        analysisManager->FillNtupleDColumn(0, 2, crystalHit.energyDeposit);
        analysisManager->FillNtupleIColumn(0, 3, static_cast<int>(detectedCount));
        analysisManager->FillNtupleIColumn(0, 4, crystalHit.generatedPhotonCount);
        analysisManager->AddNtupleRow(0);
    }

    const auto writeSensorHits{[&](const auto& sensorHits, const auto& timeOfFlight) -> void {
        fSensorIDs.clear();
        fSensorTimes.clear();
        if (sensorHits.empty()) {
            return;
        }

        fSensorIDs.reserve(sensorHits.size());
        fSensorTimes.reserve(sensorHits.size());
        for (const auto& sensorHit : sensorHits) {
            fSensorIDs.emplace_back(static_cast<int>(sensorHit.sensorID));
            fSensorTimes.emplace_back(static_cast<float>(timeOfFlight(sensorHit)));
        }
        analysisManager->FillNtupleIColumn(1, 0, static_cast<int>(sensorHits.front().eventID));
        analysisManager->AddNtupleRow(1);
    }};

    if (transportOutput != nullptr) {
        writeSensorHits(transportOutput->detections, [](const auto& detection) -> auto { return detection.timeNs; });
    } else {
        writeSensorHits(event.sensorHitOutput, [](const auto& sensorHit) -> auto { return sensorHit.timeOfFlight; });
    }
}

} // namespace G4GO::Simulation
