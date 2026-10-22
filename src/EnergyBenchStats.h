#pragma once

// Statistics for the Dock Settings energy benchmark (EnergyBench; portable, header-only): compares a
// candidate condition's repeated measurements against a baseline with a
// Welch two-sample t-test, so a reported saving is one the run-to-run noise
// cannot explain.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <vector>

namespace energybench {

[[nodiscard]] inline double Mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const double value : values) {
        sum += value;
    }
    return sum / static_cast<double>(values.size());
}

// Sample standard deviation (n - 1); 0 for fewer than two values.
[[nodiscard]] inline double StdDev(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double mean = Mean(values);
    double squares = 0.0;
    for (const double value : values) {
        squares += (value - mean) * (value - mean);
    }
    return std::sqrt(squares / static_cast<double>(values.size() - 1));
}

// Nearest-rank percentile (p in [0, 100]) of an unsorted list.
[[nodiscard]] inline double Percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double rank = std::ceil(std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(values.size()));
    const size_t index = rank < 1.0 ? 0 : static_cast<size_t>(rank) - 1;
    return values[std::min(index, values.size() - 1)];
}

// Two-sided 95% Student t critical value; fractional df rounds down (conservative).
[[nodiscard]] inline double TCritical95(double df) {
    static constexpr double kTable[] = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
        2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060,
        2.056, 2.052, 2.048, 2.045, 2.042};
    if (!(df >= 1.0)) {
        return kTable[0];
    }
    const auto whole = static_cast<size_t>(std::floor(df));
    if (whole <= std::size(kTable)) {
        return kTable[whole - 1];
    }
    return whole <= 40 ? 2.021 : whole <= 60 ? 2.000 : whole <= 120 ? 1.980 : 1.960;
}

enum class Verdict { Lower, Higher, NoDifference, NotEnoughData };

struct Comparison {
    double baselineMean = 0.0;
    double candidateMean = 0.0;
    // candidate - baseline, and its 95% confidence interval.
    double difference = 0.0;
    double ciLow = 0.0;
    double ciHigh = 0.0;
    // Relative to the baseline mean, in percent.
    double percent = 0.0;
    double percentCiLow = 0.0;
    double percentCiHigh = 0.0;
    double degreesOfFreedom = 0.0;
    Verdict verdict = Verdict::NotEnoughData;
};

// Welch's t-test of candidate against baseline. Lower/Higher only when the
// whole 95% interval of the difference lies on one side of zero.
[[nodiscard]] inline Comparison Compare(const std::vector<double>& baseline, const std::vector<double>& candidate) {
    Comparison out;
    out.baselineMean = Mean(baseline);
    out.candidateMean = Mean(candidate);
    out.difference = out.candidateMean - out.baselineMean;
    if (out.baselineMean != 0.0) {
        out.percent = out.difference / out.baselineMean * 100.0;
    }
    if (baseline.size() < 2 || candidate.size() < 2) {
        out.ciLow = out.ciHigh = out.difference;
        out.percentCiLow = out.percentCiHigh = out.percent;
        return out;
    }
    const double nb = static_cast<double>(baseline.size());
    const double nc = static_cast<double>(candidate.size());
    const double vb = StdDev(baseline) * StdDev(baseline) / nb;
    const double vc = StdDev(candidate) * StdDev(candidate) / nc;
    const double se = std::sqrt(vb + vc);
    if (se == 0.0) {
        out.degreesOfFreedom = nb + nc - 2.0;
        out.ciLow = out.ciHigh = out.difference;
    } else {
        const double denominator = vb * vb / (nb - 1.0) + vc * vc / (nc - 1.0);
        out.degreesOfFreedom = denominator > 0.0 ? (vb + vc) * (vb + vc) / denominator : nb + nc - 2.0;
        const double margin = TCritical95(out.degreesOfFreedom) * se;
        out.ciLow = out.difference - margin;
        out.ciHigh = out.difference + margin;
    }
    if (out.baselineMean != 0.0) {
        out.percentCiLow = out.ciLow / out.baselineMean * 100.0;
        out.percentCiHigh = out.ciHigh / out.baselineMean * 100.0;
    }
    out.verdict = out.ciHigh < 0.0 ? Verdict::Lower : out.ciLow > 0.0 ? Verdict::Higher : Verdict::NoDifference;
    return out;
}

}  // namespace energybench
