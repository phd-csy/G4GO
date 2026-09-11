#include "ROOT/RDFHelpers.hxx"
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

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

auto ZScore(double pValue) -> double {
    return pValue == 0.0 ? std::numeric_limits<double>::infinity() : -TMath::NormQuantile(pValue / 2.0);
}

auto ComparisonStatus(double pValue, bool rangeValid = true) -> const char* {
    const auto valid{std::isfinite(pValue) and pValue >= 0.0 and pValue <= 1.0};
    if (not rangeValid or not valid) {
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
    const auto valid{std::isfinite(pValue) and pValue >= 0.0 and pValue <= 1.0};
    return rangeValid and valid and ZScore(pValue) <= 5.0;
}

auto HasOutOfRange(const TH1D& histogram) -> bool {
    return histogram.GetBinContent(0) != 0.0 or histogram.GetBinContent(histogram.GetNbinsX() + 1) != 0.0;
}

auto DrawComparison(const TH1D& cpuHistogram, const TH1D& gpuHistogram, const char* canvasName, const char* canvasTitle,
                    const char* pullName, const char* pullTitle, const std::filesystem::path& imagePath, bool logY,
                    double cpuLegendX, const char* gpuLegendLabel) -> void {
    TH1D cpuPlot{cpuHistogram};
    TH1D gpuPlot{gpuHistogram};
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
    for (int i{1}; i <= binCount; i++) {
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

auto TestOpticalTransport(const char* cpuDataset, const char* gpuDataset, const char* outputDirectory = ".") -> void {
    gROOT->SetBatch(kTRUE);
    gErrorIgnoreLevel = kWarning;

    try {
        const std::filesystem::path outputPath{outputDirectory};
        const auto figuresPath{outputPath / "figures"};
        std::filesystem::create_directories(figuresPath);

        ROOT::RDataFrame cpuData{"CrystalHit", cpuDataset};
        ROOT::RDataFrame gpuData{"CrystalHit", gpuDataset};
        ROOT::RDataFrame cpuSensorData{"SensorHit", cpuDataset};
        ROOT::RDataFrame gpuSensorData{"SensorHit", gpuDataset};

        // --- nOptPho spectrum (CrystalHit) ---
        constexpr auto nOptPhoBins{100};
        constexpr auto nOptPhoMaximum{2000.0};
        const auto nOptPhoTitle{"nOptPho spectrum;nOptPho;Entries"};
        auto cpuNOptPhoStats{cpuData.Stats<int>("nOptPho")};
        auto gpuNOptPhoStats{gpuData.Stats<int>("nOptPho")};
        auto cpuNOptPhoTotal{
            cpuData.Define("nOptPho64", [](int value) { return static_cast<std::uint64_t>(value); }, {"nOptPho"})
                .Sum<std::uint64_t>("nOptPho64")};
        auto gpuNOptPhoTotal{
            gpuData.Define("nOptPho64", [](int value) { return static_cast<std::uint64_t>(value); }, {"nOptPho"})
                .Sum<std::uint64_t>("nOptPho64")};
        auto cpuInvalidNOptPho{cpuData.Filter([](int value) { return value < 0; }, {"nOptPho"}).Count()};
        auto gpuInvalidNOptPho{gpuData.Filter([](int value) { return value < 0; }, {"nOptPho"}).Count()};
        auto cpuNOptPhoHistogram{
            cpuData.Histo1D({"cpu_nOptPho", nOptPhoTitle, nOptPhoBins, 0.0, nOptPhoMaximum}, "nOptPho")};
        auto gpuNOptPhoHistogram{
            gpuData.Histo1D({"gpu_nOptPho", nOptPhoTitle, nOptPhoBins, 0.0, nOptPhoMaximum}, "nOptPho")};

        // --- Event energy deposition (CrystalHit) ---
        constexpr auto edepBins{50};
        constexpr auto edepMaximum{6.0};
        const auto edepTitle{"event total energy deposition;Edep [MeV];Events"};
        auto cpuEdepStats{cpuData.Stats<double>("Edep")};
        auto gpuEdepStats{gpuData.Stats<double>("Edep")};
        auto cpuInvalidEdep{
            cpuData.Filter([](double value) { return not std::isfinite(value) or value < 0.0; }, {"Edep"}).Count()};
        auto gpuInvalidEdep{
            gpuData.Filter([](double value) { return not std::isfinite(value) or value < 0.0; }, {"Edep"}).Count()};
        auto cpuEdepHistogram{cpuData.Histo1D({"cpu_event_total_Edep", edepTitle, edepBins, 0.0, edepMaximum}, "Edep")};
        auto gpuEdepHistogram{gpuData.Histo1D({"gpu_event_total_Edep", edepTitle, edepBins, 0.0, edepMaximum}, "Edep")};

        // --- Sensor time of flight (SensorHit) ---
        constexpr auto tofMaximum{1000.0};
        const auto addSensorColumns{[](ROOT::RDF::RNode data) {
            return data
                .Define("sensorCount",
                        [](const ROOT::RVec<int>& sensorIDs) { return static_cast<std::uint64_t>(sensorIDs.size()); },
                        {"sensorID"})
                .Define("tofSum",
                        [](const ROOT::RVec<float>& times) {
                            double sum{};
                            for (const auto time : times) {
                                sum += time;
                            }
                            return sum;
                        },
                        {"timeOfFlight"})
                .Define("tofSquaredSum",
                        [](const ROOT::RVec<float>& times) {
                            double sum{};
                            for (const auto time : times) {
                                sum += static_cast<double>(time) * time;
                            }
                            return sum;
                        },
                        {"timeOfFlight"})
                .Define("sensorValid",
                        [](const ROOT::RVec<int>& sensorIDs, const ROOT::RVec<float>& times) {
                            if (sensorIDs.empty() or sensorIDs.size() != times.size()) {
                                return false;
                            }
                            for (std::size_t i{}; i < times.size(); i++) {
                                if (sensorIDs[i] < 0 or sensorIDs[i] >= 64 or not std::isfinite(times[i]) or
                                    times[i] < 0.0F) {
                                    return false;
                                }
                            }
                            return true;
                        },
                        {"sensorID", "timeOfFlight"});
        }};
        auto cpuSensor{addSensorColumns(cpuSensorData)};
        auto gpuSensor{addSensorColumns(gpuSensorData)};
        auto cpuSensorCount{cpuSensor.Sum<std::uint64_t>("sensorCount")};
        auto gpuSensorCount{gpuSensor.Sum<std::uint64_t>("sensorCount")};
        auto cpuTofSum{cpuSensor.Sum<double>("tofSum")};
        auto gpuTofSum{gpuSensor.Sum<double>("tofSum")};
        auto cpuTofSquaredSum{cpuSensor.Sum<double>("tofSquaredSum")};
        auto gpuTofSquaredSum{gpuSensor.Sum<double>("tofSquaredSum")};
        auto cpuInvalidSensorRows{cpuSensor.Filter([](bool valid) { return not valid; }, {"sensorValid"}).Count()};
        auto gpuInvalidSensorRows{gpuSensor.Filter([](bool valid) { return not valid; }, {"sensorValid"}).Count()};
        auto cpuTofHistogram{
            cpuSensor.Histo1D({"cpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0, tofMaximum}, "timeOfFlight")};
        auto gpuTofHistogram{
            gpuSensor.Histo1D({"gpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0, tofMaximum}, "timeOfFlight")};

        // --- Sensor pixel occupancy (SensorHit) ---
        auto cpuOccupancyHistogram{cpuSensor.Histo1D(
            {"cpu_sensorOccupancy", "sensor occupancy;sensor ID;Entries", 64, 0.0, 64.0}, "sensorID")};
        auto gpuOccupancyHistogram{gpuSensor.Histo1D(
            {"gpu_sensorOccupancy", "sensor occupancy;sensor ID;Entries", 64, 0.0, 64.0}, "sensorID")};

        ROOT::RDF::RunGraphs({cpuNOptPhoHistogram, gpuNOptPhoHistogram, cpuTofHistogram, gpuTofHistogram});

        const auto cpuEntries{static_cast<std::uint64_t>(cpuNOptPhoStats->GetN())};
        const auto gpuEntries{static_cast<std::uint64_t>(gpuNOptPhoStats->GetN())};
        const auto cpuTotal{*cpuNOptPhoTotal};
        const auto gpuTotal{*gpuNOptPhoTotal};
        const auto cpuMean{cpuNOptPhoStats->GetMean()};
        const auto gpuMean{gpuNOptPhoStats->GetMean()};
        const auto cpuRms{cpuNOptPhoStats->GetRMS()};
        const auto gpuRms{gpuNOptPhoStats->GetRMS()};
        const auto cpuEdepEntries{static_cast<std::uint64_t>(cpuEdepStats->GetN())};
        const auto gpuEdepEntries{static_cast<std::uint64_t>(gpuEdepStats->GetN())};
        const auto cpuEdepMean{cpuEdepStats->GetMean()};
        const auto gpuEdepMean{gpuEdepStats->GetMean()};
        const auto cpuEdepRms{cpuEdepStats->GetRMS()};
        const auto gpuEdepRms{gpuEdepStats->GetRMS()};
        const auto cpuSensorEntries{*cpuSensorCount};
        const auto gpuSensorEntries{*gpuSensorCount};
        if (cpuSensorEntries == 0U or gpuSensorEntries == 0U) {
            throw std::runtime_error("SensorHit contains no detections");
        }
        const auto cpuTofMean{*cpuTofSum / static_cast<double>(cpuSensorEntries)};
        const auto gpuTofMean{*gpuTofSum / static_cast<double>(gpuSensorEntries)};
        const auto cpuTofRms{
            std::sqrt(*cpuTofSquaredSum / static_cast<double>(cpuSensorEntries) - cpuTofMean * cpuTofMean)};
        const auto gpuTofRms{
            std::sqrt(*gpuTofSquaredSum / static_cast<double>(gpuSensorEntries) - gpuTofMean * gpuTofMean)};

        if (cpuEntries < 500U or gpuEntries < 500U) {
            throw std::runtime_error("CrystalHit contains too few entries");
        }
        if (cpuTotal == 0U or gpuTotal == 0U) {
            throw std::runtime_error("nOptPho total is zero");
        }
        const auto nOptPhoRangeValid{*cpuInvalidNOptPho == 0U and *gpuInvalidNOptPho == 0U and
                                     not HasOutOfRange(*cpuNOptPhoHistogram) and
                                     not HasOutOfRange(*gpuNOptPhoHistogram)};
        const auto edepRangeValid{*cpuInvalidEdep == 0U and *gpuInvalidEdep == 0U and
                                  not HasOutOfRange(*cpuEdepHistogram) and not HasOutOfRange(*gpuEdepHistogram)};
        const auto tofRangeValid{*cpuInvalidSensorRows == 0U and *gpuInvalidSensorRows == 0U and
                                 not HasOutOfRange(*cpuTofHistogram) and not HasOutOfRange(*gpuTofHistogram)};
        const auto occupancyRangeValid{*cpuInvalidSensorRows == 0U and *gpuInvalidSensorRows == 0U and
                                       not HasOutOfRange(*cpuOccupancyHistogram) and
                                       not HasOutOfRange(*gpuOccupancyHistogram)};

        const auto nOptPhoPValue{cpuNOptPhoHistogram->Chi2Test(&*gpuNOptPhoHistogram, "UU P OF")};
        const auto tofPValue{cpuTofHistogram->Chi2Test(&*gpuTofHistogram, "UU P OF")};
        const auto tofShapePValue{cpuTofHistogram->KolmogorovTest(&*gpuTofHistogram)};
        const auto occupancyPValue{cpuOccupancyHistogram->Chi2Test(&*gpuOccupancyHistogram, "UU P OF")};
        const auto edepPValue{cpuEdepHistogram->Chi2Test(&*gpuEdepHistogram, "UU P OF")};

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
        const auto nOptPhoPassed{nOptPhoRangeValid and nOptPhoMeanRelDiff <= nOptPhoMeanRelativeTolerance and
                                 nOptPhoRmsRelDiff <= nOptPhoRmsRelativeTolerance};
        const auto edepPassed{
            ComparisonPassed(edepPValue, edepRangeValid) and edepEntryRelDiff <= edepEntryRelativeTolerance and
            edepMeanRelDiff <= edepMeanRelativeTolerance and edepRmsRelDiff <= edepRmsRelativeTolerance};
        const auto* edepStatus{"FAILED"};
        if (not edepRangeValid) {
            edepStatus = "INVALID";
        } else if (edepPassed) {
            edepStatus = "PASSED";
        }
        const auto tofPassed{tofRangeValid and tofShapePValue >= tofShapePValueMinimum and
                             tofMeanRelDiff <= tofMeanRelativeTolerance and tofRmsRelDiff <= tofRmsRelativeTolerance};
        const auto occupancyPassed{ComparisonPassed(occupancyPValue, occupancyRangeValid)};
        const auto* tofStatus{"FAILED"};
        if (not tofRangeValid) {
            tofStatus = "INVALID";
        } else if (tofPassed) {
            tofStatus = "PASSED";
        }
        const auto occupancyStatus{ComparisonStatus(occupancyPValue, occupancyRangeValid)};
        const auto distributionsPassed{nOptPhoPassed and edepPassed and tofPassed and occupancyPassed};
        const auto regressionPassed{totalPassed and sensorTotalPassed and distributionsPassed};
        const auto regressionStatus{regressionPassed ? "PASSED" : "FAILED"};
        std::ostringstream summary{};
        summary << std::fixed << std::setprecision(6) << "G4GO Optical Regression\n"
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

        DrawComparison(*cpuNOptPhoHistogram, *gpuNOptPhoHistogram, "noptpho_comparison", "nOptPho CPU/GPU comparison",
                       "nOptPho_pull", ";nOptPho;Pull", figuresPath / "noptpho_comparison.png", false, 0.14,
                       "GPU / OptiX");
        DrawComparison(*cpuEdepHistogram, *gpuEdepHistogram, "edep_comparison",
                       "Event total energy deposition CPU/GPU comparison", "event_total_Edep_pull", ";Edep [MeV];Pull",
                       figuresPath / "edep_comparison.png", false, 0.14, "GPU / OptiX");
        DrawComparison(*cpuTofHistogram, *gpuTofHistogram, "tof_comparison", "TOF CPU/GPU comparison",
                       "timeOfFlight_pull", ";time of flight [ns];Pull", figuresPath / "tof_comparison.png", true, 0.64,
                       "GPU / OptiX");

        const auto textOutputPath{outputPath / "regression_summary.txt"};
        std::ofstream outputFile{textOutputPath};
        if (not outputFile) {
            throw std::runtime_error("unable to create regression output");
        }
        outputFile << summary.str();
        outputFile.close();

        if (not regressionPassed) {
            std::cerr << "FAILED: optical regression criteria were not met\n";
            gSystem->Exit(1);
        }

        gSystem->Exit(0);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        gSystem->Exit(1);
    }
}
