#pragma once

#include "g4go/optical/PhotonTransport.hpp"

#include <cstddef>
#include <deque>
#include <vector>

namespace G4GO::Simulation {

struct CrystalHitOutput {
    int eventID{};
    int moduleID{};
    float energyDeposit{};
    int generatedPhotonCount{};
};

struct SensorHitOutput {
    int eventID{};
    int sensorID{};
    float timeOfFlight{};
};

struct AnalysisPerformance {
    double sensorPackMs{};
    double sensorNtupleMs{};
    double futureWaitMs{};
};

class Analysis final {
public:
    auto Enqueue(std::vector<CrystalHitOutput> crystalHitOutput, std::vector<SensorHitOutput> sensorHitOutput,
                 G4GO::Optical::PhotonTransportFuture transportFuture) -> void;
    auto WriteReadyEvents() -> void;
    auto WaitAndWriteNextEvent() -> void;
    auto Flush() -> void;
    auto PendingCount() const -> std::size_t { return fPendingEvents.size(); }
    auto SensorIDs() -> std::vector<int>& { return fSensorIDs; }
    auto SensorTimes() -> std::vector<float>& { return fSensorTimes; }
    auto ResetPerformance() -> void { fPerformance = {}; }
    auto Performance() const -> const AnalysisPerformance& { return fPerformance; }

private:
    struct PendingEvent {
        std::vector<CrystalHitOutput> crystalHitOutput{};
        std::vector<SensorHitOutput> sensorHitOutput{};
        G4GO::Optical::PhotonTransportFuture transportFuture{};
    };

    auto ProcessReadyEvents(bool waitForFrontEvent) -> void;
    auto WriteEvent(PendingEvent event) -> void;

    std::deque<PendingEvent> fPendingEvents{};
    std::vector<int> fSensorIDs{};
    std::vector<float> fSensorTimes{};
    AnalysisPerformance fPerformance{};
};

} // namespace G4GO::Simulation
