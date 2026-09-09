#include "ROOT/RDataFrame.hxx"
#include "TCanvas.h"
#include "TError.h"
#include "TH1D.h"
#include "TLegend.h"
#include "TLine.h"
#include "TMath.h"
#include "TPad.h"
#include "TROOT.h"
#include "TSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace {

auto ZScore(double pValue) -> double {
    return pValue == 0.0 ? std::numeric_limits<double>::infinity() : -TMath::NormQuantile(pValue / 2.0);
}

auto ComparisonStatus(double pValue, bool rangeValid = true) -> const char* {
    const auto valid{std::isfinite(pValue) && pValue >= 0.0 && pValue <= 1.0};
    if (!rangeValid || !valid) {
        return "INVALID";
    }
    const auto z{ZScore(pValue)};
    if (z > 5.0) {
        return "FAILED";
    }
    if (z > 3.0) {
        return "SUSPICIOUS";
    }
    if (z > 0.0) {
        return "PASSED";
    }
    return "IDENTICAL";
}

auto ComparisonPassed(double pValue, bool rangeValid = true) -> bool {
    const auto valid{std::isfinite(pValue) && pValue >= 0.0 && pValue <= 1.0};
    return rangeValid && valid && ZScore(pValue) <= 5.0;
}

auto HasOutOfRange(const TH1D& histogram) -> bool {
    return histogram.GetBinContent(0) != 0.0 || histogram.GetBinContent(histogram.GetNbinsX() + 1) != 0.0;
}

auto DrawComparison(TH1D* cpuHistogram, TH1D* gpuHistogram, const char* canvasName, const char* canvasTitle,
                    const char* pullName, const char* pullTitle, const std::filesystem::path& imagePath, bool logY,
                    double cpuLegendX, const char* gpuLegendLabel) -> void {
    TH1D cpuPlot{*cpuHistogram};
    TH1D gpuPlot{*gpuHistogram};
    cpuPlot.SetDirectory(nullptr);
    gpuPlot.SetDirectory(nullptr);
    // Keep one x-axis title in the combined figure. The lower pull panel
    // carries it; the upper histogram uses the same label-free baseline.
    cpuPlot.GetXaxis()->SetTitle("");
    gpuPlot.GetXaxis()->SetTitle("");
    constexpr auto axisTitleSize{0.06};
    constexpr auto axisLabelSize{0.045};
    cpuPlot.GetXaxis()->SetTitleSize(axisTitleSize);
    cpuPlot.GetYaxis()->SetTitleSize(axisTitleSize);
    gpuPlot.GetXaxis()->SetTitleSize(axisTitleSize);
    gpuPlot.GetYaxis()->SetTitleSize(axisTitleSize);
    cpuPlot.GetXaxis()->SetLabelSize(axisLabelSize);
    cpuPlot.GetYaxis()->SetLabelSize(axisLabelSize);
    gpuPlot.GetXaxis()->SetLabelSize(axisLabelSize);
    gpuPlot.GetYaxis()->SetLabelSize(axisLabelSize);

    const auto binCount{cpuPlot.GetNbinsX()};
    TH1D pull{pullName, pullTitle, binCount, cpuPlot.GetXaxis()->GetXmin(), cpuPlot.GetXaxis()->GetXmax()};
    for (auto i{1}; i <= binCount; ++i) {
        const auto difference{cpuPlot.GetBinContent(i) - gpuPlot.GetBinContent(i)};
        const auto error{std::hypot(cpuPlot.GetBinError(i), gpuPlot.GetBinError(i))};
        pull.SetBinContent(i, error == 0.0 ? 0.0 : difference / error);
        pull.SetBinError(i, 1.0);
    }
    pull.GetXaxis()->SetTitleSize(axisTitleSize);
    pull.GetYaxis()->SetTitleSize(axisTitleSize);
    pull.GetXaxis()->SetLabelSize(axisLabelSize);
    pull.GetYaxis()->SetLabelSize(axisLabelSize);

    TCanvas canvas{canvasName, canvasTitle, 1000, 800};
    TPad upperPad{"histogram_pad", "histogram_pad", 0.0, 0.4, 1.0, 1.0};
    TPad lowerPad{"pull_pad", "pull_pad", 0.0, 0.0, 1.0, 0.4};
    upperPad.SetBottomMargin(0.06);
    lowerPad.SetTopMargin(0.06);
    lowerPad.SetBottomMargin(0.20);
    upperPad.Draw();
    lowerPad.Draw();

    upperPad.cd();
    if (logY) {
        upperPad.SetLogy();
        cpuPlot.SetMinimum(0.5);
        gpuPlot.SetMinimum(0.5);
    }
    cpuPlot.SetLineColor(kRed + 1);
    gpuPlot.SetLineColor(kBlue + 1);
    cpuPlot.SetLineWidth(2);
    gpuPlot.SetLineWidth(2);
    cpuPlot.SetStats(false);
    gpuPlot.SetStats(false);
    cpuPlot.Draw("HIST");
    gpuPlot.Draw("HIST SAME");
    TLegend legend{cpuLegendX, 0.73, cpuLegendX + 0.29, 0.91};
    legend.AddEntry(&cpuPlot, "CPU / Geant4 full-core reference", "l");
    legend.AddEntry(&gpuPlot, gpuLegendLabel, "l");
    legend.Draw();

    lowerPad.cd();
    pull.SetStats(false);
    pull.Draw("HIST");
    TLine zeroLine{pull.GetXaxis()->GetXmin(), 0.0, pull.GetXaxis()->GetXmax(), 0.0};
    zeroLine.SetLineStyle(2);
    zeroLine.Draw();

    canvas.SaveAs(imagePath.c_str());
}

} // namespace

auto TestOpticalTransport(const char* cpuFileName, const char* gpuFileName, const char* outputDirectory = ".") -> void {
    gROOT->SetBatch(kTRUE);
    gErrorIgnoreLevel = kWarning;

    try {
        const std::filesystem::path outputPath{outputDirectory};
        const auto figuresPath{outputPath / "figures"};
        std::filesystem::create_directories(figuresPath);

        ROOT::RDataFrame cpuData{"CrystalHit", cpuFileName};
        ROOT::RDataFrame gpuData{"CrystalHit", gpuFileName};
        ROOT::RDataFrame cpuSensorData{"SensorHit", cpuFileName};
        ROOT::RDataFrame gpuSensorData{"SensorHit", gpuFileName};

        // One equal-width histogram per column, shared between the
        // statistical tests and the plot. Pull the raw values once for range
        // validation only.
        auto cpuNOptPhoValues{cpuData.Take<int>("nOptPho")};
        auto gpuNOptPhoValues{gpuData.Take<int>("nOptPho")};
        auto cpuEdepValues{cpuData.Take<double>("Edep")};
        auto gpuEdepValues{gpuData.Take<double>("Edep")};
        auto cpuTofValues{cpuSensorData.Take<double>("timeOfFlight")};
        auto gpuTofValues{gpuSensorData.Take<double>("timeOfFlight")};
        if (cpuNOptPhoValues->empty() || gpuNOptPhoValues->empty()) {
            throw std::runtime_error("CrystalHit contains no nOptPho values");
        }
        if (cpuEdepValues->empty() || gpuEdepValues->empty()) {
            throw std::runtime_error("CrystalHit contains no Edep values");
        }
        if (cpuTofValues->empty() || gpuTofValues->empty()) {
            throw std::runtime_error("SensorHit contains no time-of-flight values");
        }

        // --- nOptPho spectrum (CrystalHit) ---
        constexpr auto nOptPhoBins{100};
        const auto maximumNOptPho{std::max(*std::max_element(cpuNOptPhoValues->begin(), cpuNOptPhoValues->end()),
                                           *std::max_element(gpuNOptPhoValues->begin(), gpuNOptPhoValues->end()))};
        const auto nOptPhoMaximum{static_cast<double>(maximumNOptPho) + 100.0};
        const auto nOptPhoTitle{"nOptPho spectrum;nOptPho;Entries"};
        auto cpuNOptPhoHistogram{
            cpuData.Histo1D({"cpu_nOptPho", nOptPhoTitle, nOptPhoBins, 0.0, nOptPhoMaximum}, "nOptPho")};
        auto gpuNOptPhoHistogram{
            gpuData.Histo1D({"gpu_nOptPho", nOptPhoTitle, nOptPhoBins, 0.0, nOptPhoMaximum}, "nOptPho")};

        // --- Event energy deposition (CrystalHit) ---
        constexpr auto edepBins{50};
        const auto maximumEdep{std::max(*std::max_element(cpuEdepValues->begin(), cpuEdepValues->end()),
                                        *std::max_element(gpuEdepValues->begin(), gpuEdepValues->end()))};
        const auto edepMaximum{std::max(1.0, maximumEdep * 1.05)};
        const auto edepTitle{"event total energy deposition;Edep [MeV];Events"};
        auto cpuEdepHistogram{cpuData.Histo1D({"cpu_event_total_Edep", edepTitle, edepBins, 0.0, edepMaximum}, "Edep")};
        auto gpuEdepHistogram{gpuData.Histo1D({"gpu_event_total_Edep", edepTitle, edepBins, 0.0, edepMaximum}, "Edep")};

        // --- Sensor time of flight (SensorHit) ---
        const auto tofValid{std::all_of(cpuTofValues->begin(), cpuTofValues->end(),
                                        [](const auto value) { return std::isfinite(value) && value >= 0.0; }) &&
                            std::all_of(gpuTofValues->begin(), gpuTofValues->end(),
                                        [](const auto value) { return std::isfinite(value) && value >= 0.0; })};
        const auto maximumTof{std::max(*std::max_element(cpuTofValues->begin(), cpuTofValues->end()),
                                       *std::max_element(gpuTofValues->begin(), gpuTofValues->end()))};
        const auto tofMaximum{std::max(1000.0, std::ceil(maximumTof + 1.0))};
        auto cpuTofHistogram{cpuSensorData.Histo1D(
            {"cpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0, tofMaximum}, "timeOfFlight")};
        auto gpuTofHistogram{gpuSensorData.Histo1D(
            {"gpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0, tofMaximum}, "timeOfFlight")};

        // --- Sensor pixel occupancy (SensorHit) ---
        auto cpuOccupancyHistogram{cpuSensorData.Histo1D(
            {"cpu_sensorOccupancy", "sensor occupancy;sensor ID;Entries", 64, 0.0, 64.0}, "sensorID")};
        auto gpuOccupancyHistogram{gpuSensorData.Histo1D(
            {"gpu_sensorOccupancy", "sensor occupancy;sensor ID;Entries", 64, 0.0, 64.0}, "sensorID")};

        // Materialise all booked histograms.
        auto* cpuNOptPho{cpuNOptPhoHistogram.GetPtr()};
        auto* gpuNOptPho{gpuNOptPhoHistogram.GetPtr()};
        auto* cpuEdep{cpuEdepHistogram.GetPtr()};
        auto* gpuEdep{gpuEdepHistogram.GetPtr()};
        auto* cpuTof{cpuTofHistogram.GetPtr()};
        auto* gpuTof{gpuTofHistogram.GetPtr()};
        auto* cpuOccupancy{cpuOccupancyHistogram.GetPtr()};
        auto* gpuOccupancy{gpuOccupancyHistogram.GetPtr()};

        // Derive counts and moments from the materialised histograms instead
        // of re-running dataframe aggregations.
        const auto cpuEntries{static_cast<std::uint64_t>(cpuNOptPho->GetEntries())};
        const auto gpuEntries{static_cast<std::uint64_t>(gpuNOptPho->GetEntries())};
        const auto cpuTotal{
            static_cast<std::uint64_t>(std::accumulate(cpuNOptPhoValues->begin(), cpuNOptPhoValues->end(), 0LL))};
        const auto gpuTotal{
            static_cast<std::uint64_t>(std::accumulate(gpuNOptPhoValues->begin(), gpuNOptPhoValues->end(), 0LL))};
        const auto cpuMean{cpuNOptPho->GetMean()};
        const auto gpuMean{gpuNOptPho->GetMean()};
        const auto cpuRms{cpuNOptPho->GetStdDev()};
        const auto gpuRms{gpuNOptPho->GetStdDev()};
        const auto cpuEdepEntries{cpuEdepValues->size()};
        const auto gpuEdepEntries{gpuEdepValues->size()};
        const auto cpuEdepMean{cpuEdep->GetMean()};
        const auto gpuEdepMean{gpuEdep->GetMean()};
        const auto cpuEdepRms{cpuEdep->GetStdDev()};
        const auto gpuEdepRms{gpuEdep->GetStdDev()};
        const auto cpuSensorEntries{static_cast<std::uint64_t>(cpuTof->GetEntries())};
        const auto gpuSensorEntries{static_cast<std::uint64_t>(gpuTof->GetEntries())};
        const auto cpuTofMean{cpuTof->GetMean()};
        const auto gpuTofMean{gpuTof->GetMean()};
        const auto cpuTofRms{cpuTof->GetStdDev()};
        const auto gpuTofRms{gpuTof->GetStdDev()};

        if (cpuEntries < 500U || gpuEntries < 500U) {
            throw std::runtime_error("CrystalHit contains too few entries");
        }
        if (cpuTotal == 0U || gpuTotal == 0U) {
            throw std::runtime_error("nOptPho total is zero");
        }
        if (cpuSensorEntries == 0U || gpuSensorEntries == 0U) {
            throw std::runtime_error("SensorHit contains no detections");
        }
        const auto nOptPhoRangeValid{std::all_of(cpuNOptPhoValues->begin(), cpuNOptPhoValues->end(),
                                                 [](const auto value) { return value >= 0; }) &&
                                     std::all_of(gpuNOptPhoValues->begin(), gpuNOptPhoValues->end(),
                                                 [](const auto value) { return value >= 0; }) &&
                                     !HasOutOfRange(*cpuNOptPho) && !HasOutOfRange(*gpuNOptPho)};
        const auto edepRangeValid{std::all_of(cpuEdepValues->begin(), cpuEdepValues->end(),
                                              [](const auto value) { return std::isfinite(value) && value >= 0.0; }) &&
                                  std::all_of(gpuEdepValues->begin(), gpuEdepValues->end(),
                                              [](const auto value) { return std::isfinite(value) && value >= 0.0; }) &&
                                  !HasOutOfRange(*cpuEdep) && !HasOutOfRange(*gpuEdep)};
        const auto tofRangeValid{tofValid && !HasOutOfRange(*cpuTof) && !HasOutOfRange(*gpuTof)};
        const auto occupancyRangeValid{!HasOutOfRange(*cpuOccupancy) && !HasOutOfRange(*gpuOccupancy)};

        const auto nOptPhoPValue{cpuNOptPho->Chi2Test(gpuNOptPho, "UU P OF")};
        const auto tofPValue{cpuTof->Chi2Test(gpuTof, "UU P OF")};
        const auto tofShapePValue{cpuTof->KolmogorovTest(gpuTof)};
        const auto occupancyPValue{cpuOccupancy->Chi2Test(gpuOccupancy, "UU P OF")};
        const auto edepPValue{cpuEdep->Chi2Test(gpuEdep, "UU P OF")};

        const auto nOptPhoZ{ZScore(nOptPhoPValue)};
        const auto tofZ{ZScore(tofPValue)};
        const auto occupancyZ{ZScore(occupancyPValue)};
        const auto edepZ{ZScore(edepPValue)};

        const auto relativeTotalDifference{std::abs(static_cast<double>(gpuTotal) - static_cast<double>(cpuTotal)) /
                                           static_cast<double>(cpuTotal)};
        const auto relativeSensorDifference{
            std::abs(static_cast<double>(gpuSensorEntries) - static_cast<double>(cpuSensorEntries)) /
            static_cast<double>(cpuSensorEntries)};
        const auto sensorTotalPassed{relativeSensorDifference <= 0.10};
        const auto nOptPhoMeanRelDiff{std::abs(gpuMean - cpuMean) /
                                      std::max(std::abs(cpuMean), std::numeric_limits<double>::epsilon())};
        const auto nOptPhoRmsRelDiff{std::abs(gpuRms - cpuRms) /
                                     std::max(std::abs(cpuRms), std::numeric_limits<double>::epsilon())};
        const auto edepEntryRelDiff{
            std::abs(static_cast<double>(gpuEdepEntries) - static_cast<double>(cpuEdepEntries)) /
            std::max(static_cast<double>(cpuEdepEntries), std::numeric_limits<double>::epsilon())};
        const auto edepMeanRelDiff{std::abs(gpuEdepMean - cpuEdepMean) /
                                   std::max(std::abs(cpuEdepMean), std::numeric_limits<double>::epsilon())};
        const auto edepRmsRelDiff{std::abs(gpuEdepRms - cpuEdepRms) /
                                  std::max(std::abs(cpuEdepRms), std::numeric_limits<double>::epsilon())};
        const auto tofMeanRelDiff{std::abs(gpuTofMean - cpuTofMean) /
                                  std::max(std::abs(cpuTofMean), std::numeric_limits<double>::epsilon())};
        const auto tofRmsRelDiff{std::abs(gpuTofRms - cpuTofRms) /
                                 std::max(std::abs(cpuTofRms), std::numeric_limits<double>::epsilon())};

        // CPU and GPU use independent random streams after optical offload.
        // Gate the nOptPho spectrum with aggregate moments and the total-count
        // tolerance; gate the other distributions with the chi-square shape
        // test plus moment tolerances.
        constexpr auto tofShapePValueMinimum{0.01};
        constexpr auto tofMeanRelativeTolerance{0.01};
        constexpr auto tofRmsRelativeTolerance{0.05};
        constexpr auto nOptPhoMeanRelativeTolerance{0.05};
        constexpr auto nOptPhoRmsRelativeTolerance{0.10};
        constexpr auto edepEntryRelativeTolerance{0.01};
        constexpr auto edepMeanRelativeTolerance{0.02};
        constexpr auto edepRmsRelativeTolerance{0.05};

        const auto totalPassed{relativeTotalDifference <= 0.10};
        const auto nOptPhoPassed{nOptPhoRangeValid && nOptPhoMeanRelDiff <= nOptPhoMeanRelativeTolerance &&
                                 nOptPhoRmsRelDiff <= nOptPhoRmsRelativeTolerance};
        const auto edepPassed{
            ComparisonPassed(edepPValue, edepRangeValid) && edepEntryRelDiff <= edepEntryRelativeTolerance &&
            edepMeanRelDiff <= edepMeanRelativeTolerance && edepRmsRelDiff <= edepRmsRelativeTolerance};
        const auto edepStatus{!edepRangeValid ? "INVALID" : edepPassed ? "PASSED" : "FAILED"};
        const auto tofPassed{tofRangeValid && tofShapePValue >= tofShapePValueMinimum &&
                             tofMeanRelDiff <= tofMeanRelativeTolerance && tofRmsRelDiff <= tofRmsRelativeTolerance};
        const auto occupancyPassed{ComparisonPassed(occupancyPValue, occupancyRangeValid)};
        const auto tofStatus{!tofRangeValid ? "INVALID" : tofPassed ? "PASSED" : "FAILED"};
        const auto occupancyStatus{ComparisonStatus(occupancyPValue, occupancyRangeValid)};
        const auto distributionsPassed{nOptPhoPassed && edepPassed && tofPassed && occupancyPassed};
        const auto regressionPassed{totalPassed && sensorTotalPassed && distributionsPassed};
        const auto regressionStatus{regressionPassed ? "PASSED" : "FAILED"};
        std::ostringstream summary{};
        summary << std::fixed << std::setprecision(6)
                << "G4GO Optical Regression\n"
                << "=======================\n\n"
                << "Overall result\n"
                << "--------------\n"
                << "Status                 : " << regressionStatus << "\n\n"
                << "Detected optical photons\n"
                << "------------------------\n"
                << "CPU total              : " << cpuTotal << '\n'
                << "GPU total              : " << gpuTotal << '\n'
                << "Relative difference    : " << relativeTotalDifference << '\n'
                << "p-value                : " << nOptPhoPValue << '\n'
                << "z-score                : " << nOptPhoZ << '\n'
                << "Status                 : " << (nOptPhoPassed ? "PASSED" : "FAILED") << "\n\n"
                << "Energy deposition\n"
                << "-----------------\n"
                << "CPU entries            : " << cpuEdepEntries << '\n'
                << "GPU entries            : " << gpuEdepEntries << '\n'
                << "CPU mean [MeV]         : " << cpuEdepMean << '\n'
                << "GPU mean [MeV]         : " << gpuEdepMean << '\n'
                << "CPU RMS [MeV]          : " << cpuEdepRms << '\n'
                << "GPU RMS [MeV]          : " << gpuEdepRms << '\n'
                << "Mean relative diff.    : " << edepMeanRelDiff << '\n'
                << "RMS relative diff.     : " << edepRmsRelDiff << '\n'
                << "p-value                : " << edepPValue << '\n'
                << "z-score                : " << edepZ << '\n'
                << "Status                 : " << edepStatus << "\n\n"
                << "Sensor detection count\n"
                << "----------------------\n"
                << "CPU count              : " << cpuSensorEntries << '\n'
                << "GPU count              : " << gpuSensorEntries << '\n'
                << "Relative difference    : " << relativeSensorDifference << '\n'
                << "Status                 : " << (sensorTotalPassed ? "PASSED" : "FAILED") << "\n\n"
                << "Sensor time of flight\n"
                << "---------------------\n"
                << "CPU mean [ns]          : " << cpuTofMean << '\n'
                << "GPU mean [ns]          : " << gpuTofMean << '\n'
                << "CPU RMS [ns]           : " << cpuTofRms << '\n'
                << "GPU RMS [ns]           : " << gpuTofRms << '\n'
                << "Mean relative diff.    : " << tofMeanRelDiff << '\n'
                << "RMS relative diff.     : " << tofRmsRelDiff << '\n'
                << "p-value                : " << tofPValue << '\n'
                << "z-score                : " << tofZ << '\n'
                << "Shape p-value          : " << tofShapePValue << '\n'
                << "Status                 : " << tofStatus << "\n\n"
                << "Sensor pixel occupancy\n"
                << "----------------------\n"
                << "p-value                : " << occupancyPValue << '\n'
                << "z-score                : " << occupancyZ << '\n'
                << "Status                 : " << occupancyStatus << '\n';
        std::cout << summary.str();

        DrawComparison(cpuNOptPho, gpuNOptPho, "noptpho_comparison", "nOptPho CPU/GPU comparison", "nOptPho_pull",
                       ";nOptPho;Pull", figuresPath / "noptpho_comparison.png", false, 0.14, "GPU / OptiX");
        DrawComparison(cpuEdep, gpuEdep, "edep_comparison", "Event total energy deposition CPU/GPU comparison",
                       "event_total_Edep_pull", ";Edep [MeV];Pull", figuresPath / "edep_comparison.png", false, 0.14,
                       "GPU / OptiX");
        DrawComparison(cpuTof, gpuTof, "tof_comparison", "TOF CPU/GPU comparison", "timeOfFlight_pull",
                       ";time of flight [ns];Pull", figuresPath / "tof_comparison.png", true, 0.64, "GPU / OptiX");

        const auto textOutputPath{outputPath / "regression_summary.txt"};
        std::ofstream outputFile{textOutputPath};
        if (!outputFile) {
            throw std::runtime_error("unable to create regression output");
        }
        outputFile << summary.str();
        outputFile.close();

        if (!regressionPassed) {
            std::cerr << "FAILED: optical regression criteria were not met\n";
            gSystem->Exit(1);
        }

        gSystem->Exit(0);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        gSystem->Exit(1);
    }
}
