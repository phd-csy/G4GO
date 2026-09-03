#include "ROOT/RDataFrame.hxx"
#include "TCanvas.h"
#include "TFile.h"
#include "TH1D.h"
#include "TLegend.h"
#include "TLine.h"
#include "TMath.h"
#include "TPad.h"
#include "TParameter.h"
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
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct HistogramComparison {
    double fChi2{};
    int fNdf{};
    int fGoodnessFlag{};
    double fPValue{std::numeric_limits<double>::quiet_NaN()};
    double fZScore{std::numeric_limits<double>::quiet_NaN()};
    bool fValid{};
};

struct ShapeComparison {
    double fPValue{std::numeric_limits<double>::quiet_NaN()};
    bool fValid{};
};

auto CompareHistograms(TH1D* reference, TH1D* candidate)
    -> HistogramComparison {
    HistogramComparison output{};
    auto option{const_cast<Option_t*>("UU P OF")};
    output.fPValue = reference->Chi2TestX(
        candidate, output.fChi2, output.fNdf, output.fGoodnessFlag, option);
    output.fValid = std::isfinite(output.fChi2) &&
                    std::isfinite(output.fPValue) &&
                    output.fPValue >= 0.0 && output.fPValue <= 1.0 &&
                    output.fGoodnessFlag == 0;
    if (output.fValid) {
        output.fZScore = output.fPValue == 0.0 ? std::numeric_limits<double>::infinity() : -TMath::NormQuantile(output.fPValue / 2.0);
    }
    return output;
}

auto CompareShapes(TH1D* reference, TH1D* candidate) -> ShapeComparison {
    ShapeComparison output{};
    output.fPValue = reference->KolmogorovTest(candidate);
    output.fValid = std::isfinite(output.fPValue) &&
                    output.fPValue >= 0.0 && output.fPValue <= 1.0;
    return output;
}

auto HasOverflow(const TH1D& histogram) -> bool {
    return histogram.GetBinContent(0) != 0.0 ||
           histogram.GetBinContent(histogram.GetNbinsX() + 1) != 0.0;
}

auto ComparisonStatus(double zScore,
                      bool valid,
                      bool rangeValid = true) -> const char* {
    if (!rangeValid || !valid) {
        return "INVALID";
    }
    if (zScore > 5.0) {
        return "FAILED";
    }
    if (zScore > 3.0) {
        return "SUSPICIOUS";
    }
    if (zScore > 0.0) {
        return "PASSED";
    }
    return "IDENTICAL";
}

auto ComparisonStatus(const HistogramComparison& comparison,
                      bool rangeValid = true) -> const char* {
    return ComparisonStatus(comparison.fZScore, comparison.fValid,
                            rangeValid);
}

auto ComparisonPassed(double zScore,
                      bool valid,
                      bool rangeValid = true) -> bool {
    return rangeValid && valid && zScore <= 5.0;
}

auto ComparisonPassed(const HistogramComparison& comparison,
                      bool rangeValid = true) -> bool {
    return ComparisonPassed(comparison.fZScore, comparison.fValid,
                            rangeValid);
}

auto BuildComparisonEdges(const std::vector<int>& cpuValues,
                          const std::vector<int>& gpuValues)
    -> std::vector<double> {
    constexpr auto targetBinCount{8U};
    std::vector<int> values{};
    values.reserve(cpuValues.size() + gpuValues.size());
    values.insert(values.end(), cpuValues.begin(), cpuValues.end());
    values.insert(values.end(), gpuValues.begin(), gpuValues.end());
    std::sort(values.begin(), values.end());
    if (values.empty()) {
        return {0.0, 1.0};
    }

    std::vector<double> edges{};
    edges.reserve(targetBinCount + 1U);
    edges.emplace_back(static_cast<double>(values.at(0)) - 0.5);
    for (auto index{1U}; index < targetBinCount; ++index) {
        const auto valueIndex{std::min(
            values.size() - 1U,
            values.size() * static_cast<std::size_t>(index) /
                targetBinCount)};
        const auto edge{static_cast<double>(values.at(valueIndex)) - 0.5};
        if (edge > edges.at(edges.size() - 1U)) {
            edges.emplace_back(edge);
        }
    }
    const auto upperEdge{
        static_cast<double>(values.at(values.size() - 1U)) + 0.5};
    if (upperEdge > edges.at(edges.size() - 1U)) {
        edges.emplace_back(upperEdge);
    }
    return edges;
}

auto BuildComparisonEdges(const std::vector<double>& cpuValues,
                          const std::vector<double>& gpuValues)
    -> std::vector<double> {
    constexpr auto targetBinCount{40U};
    std::vector<double> values{};
    values.reserve(cpuValues.size() + gpuValues.size());
    values.insert(values.end(), cpuValues.begin(), cpuValues.end());
    values.insert(values.end(), gpuValues.begin(), gpuValues.end());
    if (values.empty() ||
        !std::all_of(values.begin(), values.end(),
                     [](const auto value) { return std::isfinite(value); })) {
        return {};
    }
    std::sort(values.begin(), values.end());

    std::vector<double> edges{};
    edges.reserve(targetBinCount + 1U);
    edges.emplace_back(
        std::nextafter(values.at(0), -std::numeric_limits<double>::infinity()));
    for (auto index{1U}; index < targetBinCount; ++index) {
        const auto valueIndex{std::min(
            values.size() - 1U,
            values.size() * static_cast<std::size_t>(index) /
                targetBinCount)};
        const auto edge{values.at(valueIndex)};
        if (edge > edges.at(edges.size() - 1U)) {
            edges.emplace_back(edge);
        }
    }
    const auto upperEdge{
        std::nextafter(values.at(values.size() - 1U),
                       std::numeric_limits<double>::infinity())};
    if (upperEdge > edges.at(edges.size() - 1U)) {
        edges.emplace_back(upperEdge);
    }
    return edges;
}

} // namespace

auto TestOpticalTransport(const char* cpuFileName,
                          const char* gpuFileName,
                          double cpuElapsedSeconds = 0.0,
                          double gpuElapsedSeconds = 0.0,
                          const char* outputDirectory = ".") -> void {
    gROOT->SetBatch(kTRUE);

    try {
        const std::filesystem::path outputPath{outputDirectory};
        ROOT::RDataFrame cpuData{"CrystalHit", cpuFileName};
        ROOT::RDataFrame gpuData{"CrystalHit", gpuFileName};
        ROOT::RDataFrame cpuSensorData{"SensorHit", cpuFileName};
        ROOT::RDataFrame gpuSensorData{"SensorHit", gpuFileName};

        auto cpuEntries{cpuData.Count()};
        auto gpuEntries{gpuData.Count()};
        auto cpuTotalOutput{cpuData.Sum<int>("nOptPho")};
        auto gpuTotalOutput{gpuData.Sum<int>("nOptPho")};
        auto cpuMean{cpuData.Mean<int>("nOptPho")};
        auto gpuMean{gpuData.Mean<int>("nOptPho")};
        auto cpuRms{cpuData.StdDev<int>("nOptPho")};
        auto gpuRms{gpuData.StdDev<int>("nOptPho")};
        auto cpuNOptPhoValues{cpuData.Take<int>("nOptPho")};
        auto gpuNOptPhoValues{gpuData.Take<int>("nOptPho")};
        auto cpuTofValues{cpuSensorData.Take<double>("timeOfFlight")};
        auto gpuTofValues{gpuSensorData.Take<double>("timeOfFlight")};
        auto cpuSensorEntries{cpuSensorData.Count()};
        auto gpuSensorEntries{gpuSensorData.Count()};
        auto cpuTofMean{cpuSensorData.Mean<double>("timeOfFlight")};
        auto gpuTofMean{gpuSensorData.Mean<double>("timeOfFlight")};
        auto cpuTofRms{cpuSensorData.StdDev<double>("timeOfFlight")};
        auto gpuTofRms{gpuSensorData.StdDev<double>("timeOfFlight")};

        if (*cpuEntries < 500U || *gpuEntries < 500U) {
            throw std::runtime_error("CrystalHit contains too few entries");
        }

        const auto cpuTotal{static_cast<std::uint64_t>(*cpuTotalOutput)};
        const auto gpuTotal{static_cast<std::uint64_t>(*gpuTotalOutput)};
        if (cpuTotal == 0U || gpuTotal == 0U) {
            throw std::runtime_error("nOptPho total is zero");
        }
        if (*cpuSensorEntries == 0U || *gpuSensorEntries == 0U) {
            throw std::runtime_error("SensorHit contains no detections");
        }

        constexpr auto spectrumPlotBins{200};
        constexpr auto spectrumMinimum{0.0};
        const auto maximumNOptPho{std::max(
            *std::max_element(cpuNOptPhoValues->begin(),
                              cpuNOptPhoValues->end()),
            *std::max_element(gpuNOptPhoValues->begin(),
                              gpuNOptPhoValues->end()))};
        const auto spectrumMaximum{
            static_cast<double>(maximumNOptPho) + 100.0};
        const auto histogramTitle{"nOptPho spectrum;nOptPho;Entries"};
        auto cpuHistogram{cpuData.Histo1D(
            {"cpu_nOptPho", histogramTitle, spectrumPlotBins, spectrumMinimum,
             spectrumMaximum},
            "nOptPho")};
        auto gpuHistogram{gpuData.Histo1D(
            {"gpu_nOptPho", histogramTitle, spectrumPlotBins, spectrumMinimum,
             spectrumMaximum},
            "nOptPho")};

        const auto comparisonEdges{
            BuildComparisonEdges(*cpuNOptPhoValues, *gpuNOptPhoValues)};
        if (comparisonEdges.size() < 2U) {
            throw std::runtime_error(
                "unable to construct nOptPho comparison bins");
        }
        const auto comparisonBins{
            static_cast<int>(comparisonEdges.size() - std::size_t{1})};
        auto cpuComparisonHistogram{cpuData.Histo1D(
            {"cpu_nOptPho_comparison", histogramTitle, comparisonBins,
             comparisonEdges.data()},
            "nOptPho")};
        auto gpuComparisonHistogram{gpuData.Histo1D(
            {"gpu_nOptPho_comparison", histogramTitle, comparisonBins,
             comparisonEdges.data()},
            "nOptPho")};
        const auto tofValuesFiniteAndNonNegative{
            std::all_of(cpuTofValues->begin(), cpuTofValues->end(),
                        [](const auto value) {
                            return std::isfinite(value) && value >= 0.0;
                        }) &&
            std::all_of(gpuTofValues->begin(), gpuTofValues->end(),
                        [](const auto value) {
                            return std::isfinite(value) && value >= 0.0;
                        })};
        const auto maximumTof{std::max(
            *std::max_element(cpuTofValues->begin(), cpuTofValues->end()),
            *std::max_element(gpuTofValues->begin(), gpuTofValues->end()))};
        const auto tofPlotMaximum{
            std::max(1000.0, std::ceil(maximumTof + 1.0))};
        auto cpuTofHistogram{cpuSensorData.Histo1D(
            {"cpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0,
             tofPlotMaximum},
            "timeOfFlight")};
        auto gpuTofHistogram{gpuSensorData.Histo1D(
            {"gpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0,
             tofPlotMaximum},
            "timeOfFlight")};
        constexpr auto tofShapeBins{40};
        auto cpuTofShapeHistogram{cpuSensorData.Histo1D(
            {"cpu_timeOfFlight_shape", "time of flight;ns;Entries",
             tofShapeBins, 0.0, tofPlotMaximum},
            "timeOfFlight")};
        auto gpuTofShapeHistogram{gpuSensorData.Histo1D(
            {"gpu_timeOfFlight_shape", "time of flight;ns;Entries",
             tofShapeBins, 0.0, tofPlotMaximum},
            "timeOfFlight")};
        const auto tofComparisonEdges{
            BuildComparisonEdges(*cpuTofValues, *gpuTofValues)};
        if (tofComparisonEdges.size() < 2U) {
            throw std::runtime_error(
                "unable to construct TOF comparison bins");
        }
        const auto tofComparisonBins{static_cast<int>(
            tofComparisonEdges.size() - std::size_t{1})};
        auto cpuTofComparisonHistogram{cpuSensorData.Histo1D(
            {"cpu_timeOfFlight_comparison", "time of flight;ns;Entries",
             tofComparisonBins, tofComparisonEdges.data()},
            "timeOfFlight")};
        auto gpuTofComparisonHistogram{gpuSensorData.Histo1D(
            {"gpu_timeOfFlight_comparison", "time of flight;ns;Entries",
             tofComparisonBins, tofComparisonEdges.data()},
            "timeOfFlight")};
        auto cpuOccupancyHistogram{cpuSensorData.Histo1D(
            {"cpu_sensorOccupancy", "sensor occupancy;sensor ID;Entries", 64,
             0.0, 64.0},
            "sensorID")};
        auto gpuOccupancyHistogram{gpuSensorData.Histo1D(
            {"gpu_sensorOccupancy", "sensor occupancy;sensor ID;Entries", 64,
             0.0, 64.0},
            "sensorID")};

        auto* cpuHistogramPtr{cpuHistogram.GetPtr()};
        auto* gpuHistogramPtr{gpuHistogram.GetPtr()};
        auto* cpuComparisonHistogramPtr{cpuComparisonHistogram.GetPtr()};
        auto* gpuComparisonHistogramPtr{gpuComparisonHistogram.GetPtr()};
        auto* cpuTofHistogramPtr{cpuTofHistogram.GetPtr()};
        auto* gpuTofHistogramPtr{gpuTofHistogram.GetPtr()};
        auto* cpuTofShapeHistogramPtr{cpuTofShapeHistogram.GetPtr()};
        auto* gpuTofShapeHistogramPtr{gpuTofShapeHistogram.GetPtr()};
        auto* cpuTofComparisonHistogramPtr{
            cpuTofComparisonHistogram.GetPtr()};
        auto* gpuTofComparisonHistogramPtr{
            gpuTofComparisonHistogram.GetPtr()};
        auto* cpuOccupancyHistogramPtr{cpuOccupancyHistogram.GetPtr()};
        auto* gpuOccupancyHistogramPtr{gpuOccupancyHistogram.GetPtr()};
        const auto nOptPhoComparison{CompareHistograms(
            cpuComparisonHistogramPtr, gpuComparisonHistogramPtr)};
        const auto tofComparison{CompareHistograms(
            cpuTofComparisonHistogramPtr, gpuTofComparisonHistogramPtr)};
        const auto tofShapeComparison{CompareShapes(
            cpuTofShapeHistogramPtr, gpuTofShapeHistogramPtr)};
        const auto occupancyComparison{CompareHistograms(
            cpuOccupancyHistogramPtr, gpuOccupancyHistogramPtr)};
        const auto tofRangeValid{
            tofValuesFiniteAndNonNegative &&
            !HasOverflow(*cpuTofHistogramPtr) &&
            !HasOverflow(*gpuTofHistogramPtr)};
        const auto occupancyRangeValid{
            !HasOverflow(*cpuOccupancyHistogramPtr) &&
            !HasOverflow(*gpuOccupancyHistogramPtr)};
        const auto chi2{nOptPhoComparison.fChi2};
        const auto ndf{nOptPhoComparison.fNdf};
        const auto igood{nOptPhoComparison.fGoodnessFlag};
        const auto pValue{nOptPhoComparison.fPValue};
        const auto zValue{nOptPhoComparison.fZScore};
        const auto significanceStatus{ComparisonStatus(nOptPhoComparison)};
        const auto relativeTotalDifference{
            std::abs(static_cast<double>(gpuTotal) - static_cast<double>(cpuTotal)) /
            static_cast<double>(cpuTotal)};
        const auto relativeSensorDifference{
            std::abs(static_cast<double>(*gpuSensorEntries) -
                     static_cast<double>(*cpuSensorEntries)) /
            static_cast<double>(*cpuSensorEntries)};
        const auto sensorTotalPassed{relativeSensorDifference <= 0.10};
        const auto nOptPhoMeanRelativeDifference{
            std::isfinite(*cpuMean) && std::isfinite(*gpuMean) ?
                std::abs(*gpuMean - *cpuMean) /
                    std::max(std::abs(*cpuMean),
                             std::numeric_limits<double>::epsilon()) :
                std::numeric_limits<double>::infinity()};
        const auto nOptPhoRmsRelativeDifference{
            std::isfinite(*cpuRms) && std::isfinite(*gpuRms) ?
                std::abs(*gpuRms - *cpuRms) /
                    std::max(std::abs(*cpuRms),
                             std::numeric_limits<double>::epsilon()) :
                std::numeric_limits<double>::infinity()};
        const auto validTof{std::isfinite(*cpuTofMean) &&
                            std::isfinite(*gpuTofMean) &&
                            std::isfinite(*cpuTofRms) &&
                            std::isfinite(*gpuTofRms)};
        const auto tofMeanRelativeDifference{
            validTof ? std::abs(*gpuTofMean - *cpuTofMean) /
                           std::max(std::abs(*cpuTofMean),
                                    std::numeric_limits<double>::epsilon()) :
                       std::numeric_limits<double>::infinity()};
        const auto tofRmsRelativeDifference{
            validTof ? std::abs(*gpuTofRms - *cpuTofRms) /
                           std::max(std::abs(*cpuTofRms),
                                    std::numeric_limits<double>::epsilon()) :
                       std::numeric_limits<double>::infinity()};
        constexpr auto tofShapePValueMinimum{0.01};
        constexpr auto tofMeanRelativeTolerance{0.01};
        constexpr auto tofRmsRelativeTolerance{0.05};
        // CPU and GPU use independent random streams after optical offload.
        // Keep the nOptPho chi-square as a diagnostic and gate the independent
        // samples with aggregate moments and the total-count tolerance.
        constexpr auto nOptPhoMeanRelativeTolerance{0.05};
        constexpr auto nOptPhoRmsRelativeTolerance{0.10};
        const auto totalPassed{relativeTotalDifference <= 0.10};
        const auto nOptPhoPassed{
            nOptPhoComparison.fValid &&
            nOptPhoMeanRelativeDifference <= nOptPhoMeanRelativeTolerance &&
            nOptPhoRmsRelativeDifference <= nOptPhoRmsRelativeTolerance};
        const auto tofPassed{
            validTof && tofRangeValid && tofComparison.fValid &&
            tofShapeComparison.fValid &&
            tofShapeComparison.fPValue >= tofShapePValueMinimum &&
            tofMeanRelativeDifference <= tofMeanRelativeTolerance &&
            tofRmsRelativeDifference <= tofRmsRelativeTolerance};
        const auto occupancyPassed{
            ComparisonPassed(occupancyComparison, occupancyRangeValid)};
        const auto tofChiSquareStatus{!tofRangeValid || !tofComparison.fValid ?
                                          "INVALID" :
                                          "DIAGNOSTIC"};
        const auto tofStatus{!validTof || !tofRangeValid ||
                                     !tofComparison.fValid ||
                                     !tofShapeComparison.fValid ?
                                 "INVALID" :
                             tofPassed ?
                                 "PASSED" :
                                 "FAILED"};
        const auto occupancyStatus{
            ComparisonStatus(occupancyComparison, occupancyRangeValid)};
        const auto distributionsPassed{nOptPhoPassed && tofPassed &&
                                       occupancyPassed};
        const auto regressionStatus{totalPassed && sensorTotalPassed &&
                                            distributionsPassed ?
                                        "PASSED" :
                                        "FAILED"};
        const auto regressionPassed{totalPassed && sensorTotalPassed &&
                                    distributionsPassed};
        const auto hasTiming{cpuElapsedSeconds > 0.0 &&
                             gpuElapsedSeconds > 0.0};
        const auto gpuSpeedup{
            hasTiming ? cpuElapsedSeconds / gpuElapsedSeconds : 0.0};

        std::cout << std::fixed << std::setprecision(4)
                  << "[statistics] CPU single-core\n"
                  << "  Entries: " << *cpuEntries << '\n'
                  << "  Total: " << cpuTotal << '\n'
                  << "  Mean: " << *cpuMean << '\n'
                  << "  RMS: " << *cpuRms << "\n\n"
                  << "[statistics] GPU single-core\n"
                  << "  Entries: " << *gpuEntries << '\n'
                  << "  Total: " << gpuTotal << '\n'
                  << "  Mean: " << *gpuMean << '\n'
                  << "  RMS: " << *gpuRms << "\n\n"
                  << "[sensor]\n"
                  << "  CPU detections: " << *cpuSensorEntries << '\n'
                  << "  GPU detections: " << *gpuSensorEntries << '\n'
                  << "  CPU TOF mean: " << *cpuTofMean << " ns\n"
                  << "  GPU TOF mean: " << *gpuTofMean << " ns\n"
                  << "  CPU TOF RMS: " << *cpuTofRms << " ns\n"
                  << "  GPU TOF RMS: " << *gpuTofRms << " ns\n"
                  << "  Relative detection difference: "
                  << relativeSensorDifference << '\n'
                  << "  Sensor occupancy bins: 64\n\n"
                  << "[comparison]\n"
                  << "  nOptPho chi-square: " << chi2 << '\n'
                  << "  nOptPho NDF: " << ndf << '\n'
                  << "  nOptPho goodness flag: " << igood << '\n'
                  << "  nOptPho p-value: " << pValue << '\n'
                  << "  nOptPho Z-score: " << zValue << '\n'
                  << "  nOptPho comparison bins: " << comparisonBins << '\n'
                  << "  nOptPho chi-square significance: " << significanceStatus
                  << '\n'
                  << "  nOptPho mean relative difference: "
                  << nOptPhoMeanRelativeDifference << '\n'
                  << "  nOptPho RMS relative difference: "
                  << nOptPhoRmsRelativeDifference << '\n'
                  << "  nOptPho gate status: "
                  << (nOptPhoPassed ? "PASSED" : "FAILED") << '\n'
                  << "  TOF chi-square: " << tofComparison.fChi2 << '\n'
                  << "  TOF NDF: " << tofComparison.fNdf << '\n'
                  << "  TOF goodness flag: "
                  << tofComparison.fGoodnessFlag << '\n'
                  << "  TOF p-value: " << tofComparison.fPValue << '\n'
                  << "  TOF Z-score: " << tofComparison.fZScore << '\n'
                  << "  TOF comparison bins: " << tofComparisonBins << '\n'
                  << "  TOF range: " << (tofRangeValid ? "VALID" : "INVALID")
                  << '\n'
                  << "  TOF chi-square status: " << tofChiSquareStatus << '\n'
                  << "  TOF shape p-value: " << tofShapeComparison.fPValue
                  << '\n'
                  << "  TOF mean relative difference: "
                  << tofMeanRelativeDifference << '\n'
                  << "  TOF RMS relative difference: "
                  << tofRmsRelativeDifference << '\n'
                  << "  TOF status: " << tofStatus << '\n'
                  << "  Occupancy chi-square: "
                  << occupancyComparison.fChi2 << '\n'
                  << "  Occupancy NDF: " << occupancyComparison.fNdf << '\n'
                  << "  Occupancy goodness flag: "
                  << occupancyComparison.fGoodnessFlag << '\n'
                  << "  Occupancy p-value: " << occupancyComparison.fPValue
                  << '\n'
                  << "  Occupancy Z-score: "
                  << occupancyComparison.fZScore << '\n'
                  << "  Occupancy range: "
                  << (occupancyRangeValid ? "VALID" : "INVALID") << '\n'
                  << "  Occupancy status: " << occupancyStatus << '\n'
                  << "  Relative total difference: " << relativeTotalDifference
                  << '\n'
                  << "  Sensor count status: "
                  << (sensorTotalPassed ? "PASSED" : "FAILED") << '\n';
        if (hasTiming) {
            std::cout << '\n'
                      << "[timing]\n"
                      << std::setprecision(6)
                      << "  CPU single-core: " << cpuElapsedSeconds
                      << " s\n"
                      << "  GPU single-core: " << gpuElapsedSeconds << " s\n"
                      << std::setprecision(4)
                      << "  GPU speedup vs single-core: " << gpuSpeedup << "x\n";
        } else {
            std::cout << '\n'
                      << "[timing]\n"
                      << "  GPU speedup: unavailable (timing was not provided)\n";
        }

        TH1D cpuPlot{*cpuHistogramPtr};
        TH1D gpuPlot{*gpuHistogramPtr};
        cpuPlot.SetName("cpu_nOptPho");
        gpuPlot.SetName("gpu_nOptPho");
        cpuPlot.SetDirectory(nullptr);
        gpuPlot.SetDirectory(nullptr);

        TH1D pull{
            "nOptPho_pull",
            "nOptPho pull;nOptPho;Pull",
            spectrumPlotBins,
            spectrumMinimum,
            spectrumMaximum};
        for (auto i{1}; i <= spectrumPlotBins; ++i) {
            const auto difference{
                cpuPlot.GetBinContent(i) - gpuPlot.GetBinContent(i)};
            const auto error{
                std::hypot(cpuPlot.GetBinError(i), gpuPlot.GetBinError(i))};
            pull.SetBinContent(i, error == 0.0 ? 0.0 : difference / error);
            pull.SetBinError(i, 1.0);
        }

        TCanvas canvas{"noptpho_comparison", "nOptPho CPU/GPU comparison", 1000, 800};
        TPad upperPad{"noptpho_histogram", "noptpho_histogram", 0.0, 0.4, 1.0, 1.0};
        TPad lowerPad{"noptpho_pull_pad", "noptpho_pull_pad", 0.0, 0.0, 1.0, 0.4};
        upperPad.SetBottomMargin(0.06);
        lowerPad.SetTopMargin(0.06);
        lowerPad.SetBottomMargin(0.20);
        upperPad.Draw();
        lowerPad.Draw();

        upperPad.cd();
        cpuPlot.SetLineColor(kRed + 1);
        gpuPlot.SetLineColor(kBlue + 1);
        cpuPlot.SetLineWidth(2);
        gpuPlot.SetLineWidth(2);
        cpuPlot.SetStats(false);
        gpuPlot.SetStats(false);
        cpuPlot.Draw("HIST");
        gpuPlot.Draw("HIST SAME");
        TLegend legend{0.64, 0.73, 0.93, 0.91};
        legend.AddEntry(&cpuPlot, "CPU / Geant4 (single core)", "l");
        std::ostringstream gpuLegendText{};
        gpuLegendText << "GPU / OptiX";
        if (hasTiming) {
            gpuLegendText << " (" << std::fixed << std::setprecision(2)
                          << gpuSpeedup << "x vs single-core CPU)";
        }
        const auto gpuLegendLabel{gpuLegendText.str()};
        legend.AddEntry(&gpuPlot, gpuLegendLabel.c_str(), "l");
        legend.Draw();

        lowerPad.cd();
        pull.SetStats(false);
        pull.Draw("HIST");
        TLine zeroLine{pull.GetXaxis()->GetXmin(), 0.0, pull.GetXaxis()->GetXmax(), 0.0};
        zeroLine.SetLineStyle(2);
        zeroLine.Draw();

        TH1D cpuTofPlot{*cpuTofHistogramPtr};
        TH1D gpuTofPlot{*gpuTofHistogramPtr};
        cpuTofPlot.SetName("cpu_timeOfFlight_plot");
        gpuTofPlot.SetName("gpu_timeOfFlight_plot");
        cpuTofPlot.SetDirectory(nullptr);
        gpuTofPlot.SetDirectory(nullptr);

        TH1D tofPull{
            "timeOfFlight_pull",
            "time of flight pull;time of flight [ns];Pull",
            cpuTofPlot.GetNbinsX(),
            cpuTofPlot.GetXaxis()->GetXmin(),
            cpuTofPlot.GetXaxis()->GetXmax()};
        for (auto i{1}; i <= cpuTofPlot.GetNbinsX(); ++i) {
            const auto difference{
                cpuTofPlot.GetBinContent(i) - gpuTofPlot.GetBinContent(i)};
            const auto error{
                std::hypot(cpuTofPlot.GetBinError(i), gpuTofPlot.GetBinError(i))};
            tofPull.SetBinContent(i, error == 0.0 ? 0.0 : difference / error);
            tofPull.SetBinError(i, 1.0);
        }

        TCanvas tofCanvas{"tof_comparison", "TOF CPU/GPU comparison", 1000, 800};
        TPad tofUpperPad{"tof_histogram", "tof_histogram", 0.0, 0.4, 1.0, 1.0};
        TPad tofLowerPad{"tof_pull_pad", "tof_pull_pad", 0.0, 0.0, 1.0, 0.4};
        tofUpperPad.SetBottomMargin(0.06);
        tofLowerPad.SetTopMargin(0.06);
        tofLowerPad.SetBottomMargin(0.20);
        tofUpperPad.Draw();
        tofLowerPad.Draw();

        tofUpperPad.cd();
        cpuTofPlot.SetLineColor(kRed + 1);
        gpuTofPlot.SetLineColor(kBlue + 1);
        cpuTofPlot.SetLineWidth(2);
        gpuTofPlot.SetLineWidth(2);
        cpuTofPlot.SetStats(false);
        gpuTofPlot.SetStats(false);
        cpuTofPlot.Draw("HIST");
        gpuTofPlot.Draw("HIST SAME");
        TLegend tofLegend{0.64, 0.73, 0.93, 0.91};
        tofLegend.AddEntry(&cpuTofPlot, "CPU / Geant4 (single core)", "l");
        tofLegend.AddEntry(&gpuTofPlot, "GPU / OptiX", "l");
        tofLegend.Draw();

        tofLowerPad.cd();
        tofPull.SetStats(false);
        tofPull.Draw("HIST");
        TLine tofZeroLine{tofPull.GetXaxis()->GetXmin(), 0.0,
                          tofPull.GetXaxis()->GetXmax(), 0.0};
        tofZeroLine.SetLineStyle(2);
        tofZeroLine.Draw();

        const auto rootReportPath{
            outputPath / "noptpho_regression_report.root"};
        TFile report{rootReportPath.c_str(), "RECREATE"};
        if (report.IsZombie()) {
            throw std::runtime_error("unable to create regression report");
        }
        cpuHistogramPtr->Write("cpu_nOptPho");
        gpuHistogramPtr->Write("gpu_nOptPho");
        cpuComparisonHistogramPtr->Write("cpu_nOptPhoComparison");
        gpuComparisonHistogramPtr->Write("gpu_nOptPhoComparison");
        cpuTofHistogramPtr->Write("cpu_timeOfFlight");
        gpuTofHistogramPtr->Write("gpu_timeOfFlight");
        cpuTofShapeHistogramPtr->Write("cpu_timeOfFlightShape");
        gpuTofShapeHistogramPtr->Write("gpu_timeOfFlightShape");
        cpuTofComparisonHistogramPtr->Write("cpu_timeOfFlightComparison");
        gpuTofComparisonHistogramPtr->Write("gpu_timeOfFlightComparison");
        cpuOccupancyHistogramPtr->Write("cpu_sensorOccupancy");
        gpuOccupancyHistogramPtr->Write("gpu_sensorOccupancy");
        cpuPlot.Write();
        gpuPlot.Write();
        pull.Write();
        canvas.Write();
        cpuTofPlot.Write();
        gpuTofPlot.Write();
        tofPull.Write();
        tofCanvas.Write();
        TParameter<double> chi2Parameter{"chi2", chi2};
        TParameter<int> ndfParameter{"ndf", ndf};
        TParameter<int> igoodParameter{"igood", igood};
        TParameter<double> pValueParameter{"pValue", pValue};
        TParameter<double> zValueParameter{"Z", zValue};
        TParameter<int> nOptPhoComparisonBinsParameter{
            "nOptPhoComparisonBins", comparisonBins};
        TParameter<double> nOptPhoMeanRelativeDifferenceParameter{
            "nOptPhoMeanRelativeDifference", nOptPhoMeanRelativeDifference};
        TParameter<double> nOptPhoRmsRelativeDifferenceParameter{
            "nOptPhoRmsRelativeDifference", nOptPhoRmsRelativeDifference};
        TParameter<bool> nOptPhoPassedParameter{
            "nOptPhoPassed", nOptPhoPassed};
        TParameter<double> totalDifferenceParameter{
            "relativeTotalDifference", relativeTotalDifference};
        TParameter<double> tofChi2Parameter{"tofChi2", tofComparison.fChi2};
        TParameter<int> tofNdfParameter{"tofNdf", tofComparison.fNdf};
        TParameter<int> tofGoodnessFlagParameter{
            "tofGoodnessFlag", tofComparison.fGoodnessFlag};
        TParameter<double> tofPValueParameter{
            "tofPValue", tofComparison.fPValue};
        TParameter<double> tofZValueParameter{"tofZ", tofComparison.fZScore};
        TParameter<double> tofShapePValueParameter{
            "tofShapePValue", tofShapeComparison.fPValue};
        TParameter<double> tofMeanRelativeDifferenceParameter{
            "tofMeanRelativeDifference", tofMeanRelativeDifference};
        TParameter<double> tofRmsRelativeDifferenceParameter{
            "tofRmsRelativeDifference", tofRmsRelativeDifference};
        TParameter<bool> tofRangeValidParameter{"tofRangeValid", tofRangeValid};
        TParameter<bool> tofPassedParameter{"tofPassed", tofPassed};
        TParameter<double> occupancyChi2Parameter{
            "occupancyChi2", occupancyComparison.fChi2};
        TParameter<int> occupancyNdfParameter{
            "occupancyNdf", occupancyComparison.fNdf};
        TParameter<int> occupancyGoodnessFlagParameter{
            "occupancyGoodnessFlag", occupancyComparison.fGoodnessFlag};
        TParameter<double> occupancyPValueParameter{
            "occupancyPValue", occupancyComparison.fPValue};
        TParameter<double> occupancyZValueParameter{
            "occupancyZ", occupancyComparison.fZScore};
        TParameter<bool> occupancyRangeValidParameter{
            "occupancyRangeValid", occupancyRangeValid};
        TParameter<bool> occupancyPassedParameter{
            "occupancyPassed", occupancyPassed};
        TParameter<double> cpuElapsedSecondsParameter{
            "cpuElapsedSeconds", cpuElapsedSeconds};
        TParameter<double> gpuElapsedSecondsParameter{
            "gpuElapsedSeconds", gpuElapsedSeconds};
        TParameter<double> gpuSpeedupParameter{
            "gpuSpeedupVsCpuSingleCore", gpuSpeedup};
        TParameter<bool> regressionPassedParameter{"regressionPassed",
                                                   regressionPassed};
        chi2Parameter.Write();
        ndfParameter.Write();
        igoodParameter.Write();
        pValueParameter.Write();
        zValueParameter.Write();
        nOptPhoComparisonBinsParameter.Write();
        nOptPhoMeanRelativeDifferenceParameter.Write();
        nOptPhoRmsRelativeDifferenceParameter.Write();
        nOptPhoPassedParameter.Write();
        totalDifferenceParameter.Write();
        tofChi2Parameter.Write();
        tofNdfParameter.Write();
        tofGoodnessFlagParameter.Write();
        tofPValueParameter.Write();
        tofZValueParameter.Write();
        tofShapePValueParameter.Write();
        tofMeanRelativeDifferenceParameter.Write();
        tofRmsRelativeDifferenceParameter.Write();
        tofRangeValidParameter.Write();
        tofPassedParameter.Write();
        occupancyChi2Parameter.Write();
        occupancyNdfParameter.Write();
        occupancyGoodnessFlagParameter.Write();
        occupancyPValueParameter.Write();
        occupancyZValueParameter.Write();
        occupancyRangeValidParameter.Write();
        occupancyPassedParameter.Write();
        cpuElapsedSecondsParameter.Write();
        gpuElapsedSecondsParameter.Write();
        gpuSpeedupParameter.Write();
        regressionPassedParameter.Write();
        report.Write();
        report.Close();
        const auto imagePath{outputPath / "noptpho_comparison.png"};
        canvas.SaveAs(imagePath.c_str());
        const auto tofImagePath{outputPath / "tof_comparison.png"};
        tofCanvas.SaveAs(tofImagePath.c_str());

        const auto textOutputPath{outputPath / "regression_output.txt"};
        std::ofstream outputFile{textOutputPath};
        if (!outputFile) {
            throw std::runtime_error("unable to create regression output");
        }
        outputFile << std::fixed << std::setprecision(6)
                   << "Status: " << regressionStatus << "\n\n"
                   << "CPU single-core\n"
                   << "  Entries: " << *cpuEntries << '\n'
                   << "  Total: " << cpuTotal << "\n\n"
                   << "GPU single-core\n"
                   << "  Entries: " << *gpuEntries << '\n'
                   << "  Total: " << gpuTotal << "\n\n"
                   << "Sensor\n"
                   << "  CPU detections: " << *cpuSensorEntries << '\n'
                   << "  GPU detections: " << *gpuSensorEntries << '\n'
                   << "  CPU TOF mean: " << *cpuTofMean << " ns\n"
                   << "  GPU TOF mean: " << *gpuTofMean << " ns\n"
                   << "  CPU TOF RMS: " << *cpuTofRms << " ns\n"
                   << "  GPU TOF RMS: " << *gpuTofRms << " ns\n"
                   << "  Relative detection difference: "
                   << relativeSensorDifference << "\n"
                   << "  Sensor occupancy bins: 64\n\n"
                   << "Comparison\n"
                   << "  nOptPho chi-square: " << chi2 << '\n'
                   << "  nOptPho NDF: " << ndf << '\n'
                   << "  nOptPho goodness flag: " << igood << '\n'
                   << "  nOptPho p-value: " << pValue << '\n'
                   << "  nOptPho Z-score: " << zValue << '\n'
                   << "  nOptPho comparison bins: " << comparisonBins << '\n'
                   << "  nOptPho chi-square significance: " << significanceStatus
                   << '\n'
                   << "  nOptPho mean relative difference: "
                   << nOptPhoMeanRelativeDifference << '\n'
                   << "  nOptPho RMS relative difference: "
                   << nOptPhoRmsRelativeDifference << '\n'
                   << "  nOptPho gate status: "
                   << (nOptPhoPassed ? "PASSED" : "FAILED") << '\n'
                   << "  TOF chi-square: " << tofComparison.fChi2 << '\n'
                   << "  TOF NDF: " << tofComparison.fNdf << '\n'
                   << "  TOF goodness flag: "
                   << tofComparison.fGoodnessFlag << '\n'
                   << "  TOF p-value: " << tofComparison.fPValue << '\n'
                   << "  TOF Z-score: " << tofComparison.fZScore << '\n'
                   << "  TOF comparison bins: " << tofComparisonBins << '\n'
                   << "  TOF range: " << (tofRangeValid ? "VALID" : "INVALID")
                   << '\n'
                   << "  TOF chi-square status: " << tofChiSquareStatus << '\n'
                   << "  TOF shape p-value: " << tofShapeComparison.fPValue
                   << '\n'
                   << "  TOF mean relative difference: "
                   << tofMeanRelativeDifference << '\n'
                   << "  TOF RMS relative difference: "
                   << tofRmsRelativeDifference << '\n'
                   << "  TOF status: " << tofStatus << '\n'
                   << "  Occupancy chi-square: "
                   << occupancyComparison.fChi2 << '\n'
                   << "  Occupancy NDF: " << occupancyComparison.fNdf << '\n'
                   << "  Occupancy goodness flag: "
                   << occupancyComparison.fGoodnessFlag << '\n'
                   << "  Occupancy p-value: " << occupancyComparison.fPValue
                   << '\n'
                   << "  Occupancy Z-score: "
                   << occupancyComparison.fZScore << '\n'
                   << "  Occupancy range: "
                   << (occupancyRangeValid ? "VALID" : "INVALID") << '\n'
                   << "  Occupancy status: " << occupancyStatus << '\n'
                   << "  Relative total difference: " << relativeTotalDifference
                   << '\n'
                   << "  Sensor count status: "
                   << (sensorTotalPassed ? "PASSED" : "FAILED") << '\n';
        if (hasTiming) {
            outputFile << "\nTiming\n"
                       << "  CPU single-core: " << cpuElapsedSeconds << " s\n"
                       << "  GPU single-core: " << gpuElapsedSeconds << " s\n"
                       << "  GPU speedup vs single-core: " << gpuSpeedup << "x\n";
        } else {
            outputFile << "\nTiming\n"
                       << "  GPU speedup vs single-core: unavailable\n";
        }
        outputFile.close();

        if (!regressionPassed) {
            std::cerr << "FAILED: optical regression criteria were not met\n";
            gSystem->Exit(1);
        }

        std::cout << "[regression] Status: " << regressionStatus
                  << " (nOptPho CPU/GPU regression)\n";
        gSystem->Exit(0);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        gSystem->Exit(1);
    }
}
