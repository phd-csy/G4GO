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

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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

        auto cpuEntries{cpuData.Count()};
        auto gpuEntries{gpuData.Count()};
        auto cpuTotalResult{cpuData.Sum<int>("nOptPho")};
        auto gpuTotalResult{gpuData.Sum<int>("nOptPho")};
        auto cpuMean{cpuData.Mean<int>("nOptPho")};
        auto gpuMean{gpuData.Mean<int>("nOptPho")};
        auto cpuRms{cpuData.StdDev<int>("nOptPho")};
        auto gpuRms{gpuData.StdDev<int>("nOptPho")};

        if (*cpuEntries < 500U || *gpuEntries < 500U) {
            throw std::runtime_error("CrystalHit contains too few entries");
        }

        const auto cpuTotal{static_cast<std::uint64_t>(*cpuTotalResult)};
        const auto gpuTotal{static_cast<std::uint64_t>(*gpuTotalResult)};
        if (cpuTotal == 0U || gpuTotal == 0U) {
            throw std::runtime_error("nOptPho total is zero");
        }

        constexpr auto nBins{200};
        constexpr auto spectrumMinimum{0.0};
        constexpr auto spectrumMaximum{1000.0};
        const auto histogramTitle{"nOptPho spectrum;nOptPho;Entries"};
        auto cpuHistogram{cpuData.Histo1D(
            {"cpu_nOptPho", histogramTitle, nBins, spectrumMinimum,
             spectrumMaximum},
            "nOptPho")};
        auto gpuHistogram{gpuData.Histo1D(
            {"gpu_nOptPho", histogramTitle, nBins, spectrumMinimum,
             spectrumMaximum},
            "nOptPho")};

        auto* cpuHistogramPtr{cpuHistogram.GetPtr()};
        auto* gpuHistogramPtr{gpuHistogram.GetPtr()};
        const auto pValue{cpuHistogramPtr->Chi2Test(gpuHistogramPtr, "P")};
        const auto zValue{TMath::NormQuantile(1.0 - pValue / 2.0)};
        const auto significanceStatus{
            zValue > 5.0 ? "FAILED" :
            zValue > 3.0 ? "SUSPICIOUS" :
            zValue > 0.0 ? "PASSED" :
                           "IDENTICAL"};
        const auto relativeTotalDifference{
            std::abs(static_cast<double>(gpuTotal) - static_cast<double>(cpuTotal)) /
            static_cast<double>(cpuTotal)};
        const auto totalPassed{relativeTotalDifference <= 0.10};
        const auto regressionStatus{totalPassed ? significanceStatus : "FAILED"};
        const auto regressionPassed{totalPassed && zValue <= 5.0};
        const auto hasTiming{cpuElapsedSeconds > 0.0 &&
                             gpuElapsedSeconds > 0.0};
        const auto gpuSpeedup{hasTiming ? cpuElapsedSeconds /
                                              gpuElapsedSeconds :
                                          0.0};

        std::cout << std::fixed << std::setprecision(4)
                  << "CPU: entries=" << *cpuEntries
                  << ", nOptPho_total=" << cpuTotal
                  << ", nOptPho_mean=" << *cpuMean
                  << ", nOptPho_rms=" << *cpuRms << '\n'
                  << "GPU: entries=" << *gpuEntries
                  << ", nOptPho_total=" << gpuTotal
                  << ", nOptPho_mean=" << *gpuMean
                  << ", nOptPho_rms=" << *gpuRms << '\n'
                  << "pValue=" << pValue << ", Z=" << zValue
                  << ", significance=" << significanceStatus
                  << ", relative_nOptPho_total_difference="
                  << relativeTotalDifference << '\n';
        if (hasTiming) {
            std::cout << std::setprecision(6)
                      << "CPU wall time (s): " << cpuElapsedSeconds << '\n'
                      << "GPU wall time (s): " << gpuElapsedSeconds << '\n'
                      << std::setprecision(4)
                      << "GPU speedup: " << gpuSpeedup << "x\n";
        } else {
            std::cout << "GPU speedup: unavailable (timing was not provided)\n";
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
            nBins,
            spectrumMinimum,
            spectrumMaximum};
        for (auto i{1}; i <= nBins; ++i) {
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
        legend.AddEntry(&cpuPlot, "CPU / Geant4", "l");
        std::ostringstream gpuLegendText{};
        gpuLegendText << "GPU / OptiX";
        if (hasTiming) {
            gpuLegendText << " (" << std::fixed << std::setprecision(2)
                          << gpuSpeedup << "x)";
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
        cpuPlot.Write();
        gpuPlot.Write();
        pull.Write();
        canvas.Write();
        TParameter<double> pValueParameter{"pValue", pValue};
        TParameter<double> zValueParameter{"Z", zValue};
        TParameter<double> totalDifferenceParameter{
            "relativeTotalDifference", relativeTotalDifference};
        TParameter<double> cpuElapsedSecondsParameter{
            "cpuElapsedSeconds", cpuElapsedSeconds};
        TParameter<double> gpuElapsedSecondsParameter{
            "gpuElapsedSeconds", gpuElapsedSeconds};
        TParameter<double> gpuSpeedupParameter{"gpuSpeedup", gpuSpeedup};
        TParameter<bool> regressionPassedParameter{"regressionPassed",
                                                   regressionPassed};
        pValueParameter.Write();
        zValueParameter.Write();
        totalDifferenceParameter.Write();
        cpuElapsedSecondsParameter.Write();
        gpuElapsedSecondsParameter.Write();
        gpuSpeedupParameter.Write();
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
               << "status=" << regressionStatus
               << '\n'
               << "cpu_entries=" << *cpuEntries << '\n'
               << "gpu_entries=" << *gpuEntries << '\n'
               << "cpu_nOptPho_total=" << cpuTotal << '\n'
               << "gpu_nOptPho_total=" << gpuTotal << '\n'
               << "pValue=" << pValue << '\n'
               << "Z=" << zValue << '\n'
               << "significance=" << significanceStatus << '\n'
               << "relative_nOptPho_total_difference="
               << relativeTotalDifference << '\n';
        if (hasTiming) {
            result << "cpu_wall_time_seconds=" << cpuElapsedSeconds << '\n'
                   << "gpu_wall_time_seconds=" << gpuElapsedSeconds << '\n'
                   << "gpu_speedup=" << gpuSpeedup << "x\n";
        } else {
            result << "gpu_speedup=unavailable\n";
        }
        result << "comparison_image=" << imagePath.string() << '\n'
               << "comparison_report=" << reportPath.string() << '\n';
        result.close();

        if (!regressionPassed) {
            std::cerr << "FAILED: nOptPho regression criteria were not met\n";
            gSystem->Exit(1);
        }

        std::cout << regressionStatus << ": nOptPho CPU/GPU regression\n";
        gSystem->Exit(0);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        gSystem->Exit(1);
    }
}
