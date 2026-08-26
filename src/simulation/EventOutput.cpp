#include "g4go/simulation/EventOutput.hpp"

#include "G4AnalysisManager.hh"

#include <chrono>
#include <future>
#include <utility>

namespace G4GO::Simulation {

auto EventOutputQueue::Submit(PendingEventOutput output) -> void {
    fPending.push_back(std::move(output));
}

auto EventOutputQueue::DrainReady(bool waitForOne) -> void {
    if (fPending.empty()) {
        return;
    }
    if (waitForOne) {
        fPending.front().fTransport.wait();
    }
    while (!fPending.empty()) {
        auto& output{fPending.front()};
        if (output.fTransport.valid() &&
            output.fTransport.wait_for(std::chrono::seconds{0}) !=
                std::future_status::ready) {
            break;
        }
        auto completed{std::move(output)};
        fPending.pop_front();
        Write(std::move(completed));
    }
}

auto EventOutputQueue::Flush() -> void {
    while (!fPending.empty()) {
        DrainReady(true);
    }
}

auto EventOutputQueue::Write(PendingEventOutput output) -> void {
    G4GO::Optical::TransportResult result{};
    if (output.fTransport.valid()) {
        result = output.fTransport.get();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    for (const auto& crystalHit : output.fCrystalHits) {
        analysisManager->FillNtupleIColumn(0, 0, crystalHit.fEventID);
        analysisManager->FillNtupleIColumn(0, 1, crystalHit.fModuleID);
        analysisManager->FillNtupleDColumn(0, 2, crystalHit.fEnergyDeposit);
        analysisManager->FillNtupleIColumn(
            0, 3, static_cast<int>(result.fStats.fDetectedCount));
        analysisManager->AddNtupleRow(0);
    }

    for (const auto& photonHit : result.fHitData) {
        analysisManager->FillNtupleIColumn(
            1, 0, static_cast<int>(photonHit.fEventID));
        analysisManager->FillNtupleIColumn(
            1, 1, static_cast<int>(photonHit.fSensorID));
        analysisManager->FillNtupleDColumn(1, 2, photonHit.fTimeNs);
        analysisManager->AddNtupleRow(1);
    }
}

} // namespace G4GO::Simulation
