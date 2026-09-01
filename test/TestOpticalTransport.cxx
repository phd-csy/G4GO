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

auto TestOpticalTransport(const char* cpuSingleFileName,
                          const char* cpuAllFileName,
                          const char* gpuFileName,
                          double cpuSingleElapsedSeconds = 0.0,
                          double cpuAllElapsedSeconds = 0.0,
                          double gpuElapsedSeconds = 0.0,
                          const char* outputDirectory = ".") -> void {
    gROOT->SetBatch(kTRUE);

    try {
        const std::filesystem::path outputPath{outputDirectory};
        ROOT::RDataFrame cpuSingleData{"CrystalHit", cpuSingleFileName};
        ROOT::RDataFrame cpuData{"CrystalHit", cpuAllFileName};
        ROOT::RDataFrame gpuData{"CrystalHit", gpuFileName};
        ROOT::RDataFrame cpuSensorData{"SensorHit", cpuAllFileName};
        ROOT::RDataFrame gpuSensorData{"SensorHit", gpuFileName};

        auto cpuSingleEntries{cpuSingleData.Count()};
        auto cpuSingleTotalResult{cpuSingleData.Sum<int>("nOptPho")};
        auto cpuSingleMean{cpuSingleData.Mean<int>("nOptPho")};
        auto cpuSingleRms{cpuSingleData.StdDev<int>("nOptPho")};
        auto cpuSingleMaximumResult{cpuSingleData.Max<int>("nOptPho")};
        auto cpuEntries{cpuData.Count()};
        auto gpuEntries{gpuData.Count()};
        auto cpuTotalResult{cpuData.Sum<int>("nOptPho")};
        auto gpuTotalResult{gpuData.Sum<int>("nOptPho")};
        auto cpuMean{cpuData.Mean<int>("nOptPho")};
        auto gpuMean{gpuData.Mean<int>("nOptPho")};
        auto cpuRms{cpuData.StdDev<int>("nOptPho")};
        auto gpuRms{gpuData.StdDev<int>("nOptPho")};
        auto cpuMaximumResult{cpuData.Max<int>("nOptPho")};
        auto gpuMaximumResult{gpuData.Max<int>("nOptPho")};
        auto cpuSensorEntries{cpuSensorData.Count()};
        auto gpuSensorEntries{gpuSensorData.Count()};
        auto cpuTofMean{cpuSensorData.Mean<double>("timeOfFlight")};
        auto gpuTofMean{gpuSensorData.Mean<double>("timeOfFlight")};
        auto cpuTofRms{cpuSensorData.StdDev<double>("timeOfFlight")};
        auto gpuTofRms{gpuSensorData.StdDev<double>("timeOfFlight")};

        if (*cpuSingleEntries < 500U || *cpuEntries < 500U ||
            *gpuEntries < 500U) {
            throw std::runtime_error("CrystalHit contains too few entries");
        }

        const auto cpuSingleTotal{
            static_cast<std::uint64_t>(*cpuSingleTotalResult)};
        const auto cpuTotal{static_cast<std::uint64_t>(*cpuTotalResult)};
        const auto gpuTotal{static_cast<std::uint64_t>(*gpuTotalResult)};
        if (cpuSingleTotal == 0U || cpuTotal == 0U || gpuTotal == 0U) {
            throw std::runtime_error("nOptPho total is zero");
        }
        if (*cpuSensorEntries == 0U || *gpuSensorEntries == 0U) {
            throw std::runtime_error("SensorHit contains no detections");
        }

        constexpr auto plotBins{100};
        constexpr auto spectrumMinimum{0.0};
        constexpr auto spectrumMaximum{55.0};
        const auto histogramTitle{"nOptPho spectrum;nOptPho;Entries"};
        auto cpuHistogram{cpuData.Histo1D(
            {"cpu_nOptPho", histogramTitle, plotBins, spectrumMinimum,
             spectrumMaximum},
            "nOptPho")};
        auto gpuHistogram{gpuData.Histo1D(
            {"gpu_nOptPho", histogramTitle, plotBins, spectrumMinimum,
             spectrumMaximum},
            "nOptPho")};

        const auto maximumNOptPho{std::max(
            {*cpuSingleMaximumResult, *cpuMaximumResult, *gpuMaximumResult})};
        const auto comparisonUpperEdge{
            static_cast<double>(maximumNOptPho) + 1.0};
        constexpr auto comparisonBinCount{4};
        std::vector<double> comparisonEdges{};
        comparisonEdges.reserve(comparisonBinCount + 1);
        for (auto index{0}; index <= comparisonBinCount; ++index) {
            comparisonEdges.push_back(
                comparisonUpperEdge * static_cast<double>(index) /
                static_cast<double>(comparisonBinCount));
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
        auto cpuTofHistogram{cpuSensorData.Histo1D(
            {"cpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0,
             1000.0},
            "timeOfFlight")};
        auto gpuTofHistogram{gpuSensorData.Histo1D(
            {"gpu_timeOfFlight", "time of flight;ns;Entries", 100, 0.0,
             1000.0},
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
        auto* cpuOccupancyHistogramPtr{cpuOccupancyHistogram.GetPtr()};
        auto* gpuOccupancyHistogramPtr{gpuOccupancyHistogram.GetPtr()};
        double chi2{};
        int ndf{};
        int igood{};
        auto option{const_cast<Option_t*>("UU P OF")};
        const auto pValue{cpuComparisonHistogramPtr->Chi2TestX(
            gpuComparisonHistogramPtr, chi2, ndf, igood, option)};
        const auto validPValue{std::isfinite(pValue) && pValue >= 0.0 &&
                               pValue <= 1.0};
        const auto validChiSquare{igood == 0};
        double zValue{std::numeric_limits<double>::quiet_NaN()};
        if (validPValue && validChiSquare) {
            zValue = pValue == 0.0 ? std::numeric_limits<double>::infinity() : -TMath::NormQuantile(pValue / 2.0);
        }
        const auto significanceStatus{
            !validPValue || !validChiSquare ? "INVALID" :
            zValue > 5.0                    ? "FAILED" :
            zValue > 3.0                    ? "SUSPICIOUS" :
            zValue > 0.0                    ? "PASSED" :
                                              "IDENTICAL"};
        const auto relativeTotalDifference{
            std::abs(static_cast<double>(gpuTotal) - static_cast<double>(cpuTotal)) /
            static_cast<double>(cpuTotal)};
        const auto relativeSensorDifference{
            std::abs(static_cast<double>(*gpuSensorEntries) -
                     static_cast<double>(*cpuSensorEntries)) /
            static_cast<double>(*cpuSensorEntries)};
        const auto sensorTotalPassed{relativeSensorDifference <= 0.10};
        const auto validTof{std::isfinite(*cpuTofMean) &&
                            std::isfinite(*gpuTofMean) &&
                            std::isfinite(*cpuTofRms) &&
                            std::isfinite(*gpuTofRms)};
        const auto totalPassed{relativeTotalDifference <= 0.10};
        const auto regressionStatus{totalPassed && sensorTotalPassed &&
                                            validTof ?
                                        significanceStatus :
                                        "FAILED"};
        const auto regressionPassed{totalPassed && sensorTotalPassed &&
                                    validTof && zValue <= 5.0};
        const auto hasTiming{cpuSingleElapsedSeconds > 0.0 &&
                             cpuAllElapsedSeconds > 0.0 &&
                             gpuElapsedSeconds > 0.0};
        const auto cpuSingleSpeedup{hasTiming ? cpuSingleElapsedSeconds /
                                                    gpuElapsedSeconds :
                                                0.0};
        const auto cpuAllSpeedup{hasTiming ? cpuAllElapsedSeconds /
                                                 gpuElapsedSeconds :
                                             0.0};

        std::cout << std::fixed << std::setprecision(4)
                  << "[statistics] CPU single-core\n"
                  << "  Entries: " << *cpuSingleEntries << '\n'
                  << "  Total: " << cpuSingleTotal << '\n'
                  << "  Mean: " << *cpuSingleMean << '\n'
                  << "  RMS: " << *cpuSingleRms << "\n\n"
                  << "[statistics] CPU all-core\n"
                  << "  Entries: " << *cpuEntries << '\n'
                  << "  Total: " << cpuTotal << '\n'
                  << "  Mean: " << *cpuMean << '\n'
                  << "  RMS: " << *cpuRms << "\n\n"
                  << "[statistics] GPU\n"
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
                  << "  Chi-square: " << chi2 << '\n'
                  << "  NDF: " << ndf << '\n'
                  << "  Goodness flag: " << igood << '\n'
                  << "  p-value: " << pValue << '\n'
                  << "  Z-score: " << zValue << '\n'
                  << "  Significance: " << significanceStatus << '\n'
                  << "  Relative total difference: " << relativeTotalDifference
                  << '\n'
                  << "  Sensor count status: "
                  << (sensorTotalPassed ? "PASSED" : "FAILED") << '\n';
        if (hasTiming) {
            std::cout << '\n'
                      << "[timing]\n"
                      << std::setprecision(6)
                      << "  CPU single-core: " << cpuSingleElapsedSeconds
                      << " s\n"
                      << "  CPU all-core: " << cpuAllElapsedSeconds << " s\n"
                      << "  GPU: " << gpuElapsedSeconds << " s\n"
                      << std::setprecision(4)
                      << "  GPU speedup vs single-core: " << cpuSingleSpeedup
                      << "x\n"
                      << "  GPU speedup vs all-core: " << cpuAllSpeedup << "x\n";
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
            plotBins,
            spectrumMinimum,
            spectrumMaximum};
        for (auto i{1}; i <= plotBins; ++i) {
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
        legend.AddEntry(&cpuPlot, "CPU / Geant4 (all cores)", "l");
        std::ostringstream gpuLegendText{};
        gpuLegendText << "GPU / OptiX";
        if (hasTiming) {
            gpuLegendText << " (" << std::fixed << std::setprecision(2)
                          << cpuAllSpeedup << "x vs all-core CPU)";
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

        const auto reportPath{outputPath / "noptpho_regression_report.root"};
        TFile report{reportPath.c_str(), "RECREATE"};
        if (report.IsZombie()) {
            throw std::runtime_error("unable to create regression report");
        }
        cpuHistogramPtr->Write("cpu_nOptPho");
        gpuHistogramPtr->Write("gpu_nOptPho");
        cpuTofHistogramPtr->Write("cpu_timeOfFlight");
        gpuTofHistogramPtr->Write("gpu_timeOfFlight");
        cpuOccupancyHistogramPtr->Write("cpu_sensorOccupancy");
        gpuOccupancyHistogramPtr->Write("gpu_sensorOccupancy");
        cpuPlot.Write();
        gpuPlot.Write();
        pull.Write();
        canvas.Write();
        TParameter<double> chi2Parameter{"chi2", chi2};
        TParameter<int> ndfParameter{"ndf", ndf};
        TParameter<int> igoodParameter{"igood", igood};
        TParameter<double> pValueParameter{"pValue", pValue};
        TParameter<double> zValueParameter{"Z", zValue};
        TParameter<double> totalDifferenceParameter{
            "relativeTotalDifference", relativeTotalDifference};
        TParameter<double> cpuAllElapsedSecondsParameter{
            "cpuAllElapsedSeconds", cpuAllElapsedSeconds};
        TParameter<double> cpuSingleElapsedSecondsParameter{
            "cpuSingleElapsedSeconds", cpuSingleElapsedSeconds};
        TParameter<double> gpuElapsedSecondsParameter{
            "gpuElapsedSeconds", gpuElapsedSeconds};
        TParameter<double> gpuSpeedupVsCpuSingleParameter{
            "gpuSpeedupVsCpuSingleCore", cpuSingleSpeedup};
        TParameter<double> gpuSpeedupVsCpuAllParameter{
            "gpuSpeedupVsCpuAllCore", cpuAllSpeedup};
        TParameter<bool> regressionPassedParameter{"regressionPassed",
                                                   regressionPassed};
        chi2Parameter.Write();
        ndfParameter.Write();
        igoodParameter.Write();
        pValueParameter.Write();
        zValueParameter.Write();
        totalDifferenceParameter.Write();
        cpuAllElapsedSecondsParameter.Write();
        gpuElapsedSecondsParameter.Write();
        gpuSpeedupVsCpuSingleParameter.Write();
        gpuSpeedupVsCpuAllParameter.Write();
        regressionPassedParameter.Write();
        report.Write();
        report.Close();
        const auto imagePath{outputPath / "noptpho_comparison.png"};
        canvas.SaveAs(imagePath.c_str());

        const auto resultPath{outputPath / "regression_result.txt"};
        std::ofstream result{resultPath};
        if (!result) {
            throw std::runtime_error("unable to create regression result");
        }
        result << std::fixed << std::setprecision(6)
               << "Status: " << regressionStatus << "\n\n"
               << "CPU single-core\n"
               << "  Entries: " << *cpuSingleEntries << '\n'
               << "  Total: " << cpuSingleTotal << "\n\n"
               << "CPU all-core\n"
               << "  Entries: " << *cpuEntries << '\n'
               << "  Total: " << cpuTotal << "\n\n"
               << "GPU\n"
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
               << "  Chi-square: " << chi2 << '\n'
               << "  NDF: " << ndf << '\n'
               << "  Goodness flag: " << igood << '\n'
               << "  p-value: " << pValue << '\n'
               << "  Z-score: " << zValue << '\n'
               << "  Significance: " << significanceStatus << '\n'
               << "  Relative total difference: " << relativeTotalDifference
               << '\n'
               << "  Sensor count status: "
               << (sensorTotalPassed ? "PASSED" : "FAILED") << '\n';
        if (hasTiming) {
            result << "\nTiming\n"
                   << "  CPU single-core: " << cpuSingleElapsedSeconds << " s\n"
                   << "  CPU all-core: " << cpuAllElapsedSeconds << " s\n"
                   << "  GPU: " << gpuElapsedSeconds << " s\n"
                   << "  GPU speedup vs single-core: " << cpuSingleSpeedup
                   << "x\n"
                   << "  GPU speedup vs all-core: " << cpuAllSpeedup << "x\n";
        } else {
            result << "\nTiming\n"
                   << "  GPU speedup vs single-core: unavailable\n"
                   << "  GPU speedup vs all-core: unavailable\n";
        }
        result.close();

        if (!regressionPassed) {
            std::cerr << "FAILED: nOptPho regression criteria were not met\n";
            gSystem->Exit(1);
        }

        std::cout << "[result] Status: " << regressionStatus
                  << " (nOptPho CPU/GPU regression)\n";
        gSystem->Exit(0);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        gSystem->Exit(1);
    }
}
