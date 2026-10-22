// EnergyBenchStats unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "EnergyBenchStats.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (false)

bool Near(double a, double b, double tolerance = 1e-3) {
    return std::fabs(a - b) <= tolerance;
}

void TestBasics() {
    using namespace energybench;
    EXPECT(Mean({}) == 0.0);
    EXPECT(Near(Mean({1.0, 2.0, 3.0, 4.0}), 2.5));
    EXPECT(StdDev({5.0}) == 0.0);
    EXPECT(Near(StdDev({2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0}), 2.13809));
    EXPECT(Percentile({}, 95.0) == 0.0);
    EXPECT(Percentile({5.0, 1.0, 3.0, 2.0, 4.0}, 50.0) == 3.0);
    EXPECT(Percentile({5.0, 1.0, 3.0, 2.0, 4.0}, 95.0) == 5.0);
    EXPECT(Percentile({5.0, 1.0, 3.0}, 0.0) == 1.0);
    EXPECT(Near(TCritical95(1.0), 12.706));
    EXPECT(Near(TCritical95(4.7), 2.776));
    EXPECT(Near(TCritical95(30.0), 2.042));
    EXPECT(Near(TCritical95(1000.0), 1.960));
    EXPECT(Near(TCritical95(0.2), 12.706));
}

void TestCompare() {
    using namespace energybench;
    // Clear saving: ~20% lower with small noise.
    const Comparison lower = Compare({100.0, 102.0, 98.0, 101.0}, {80.0, 81.0, 79.0, 80.5});
    EXPECT(lower.verdict == Verdict::Lower);
    EXPECT(Near(lower.percent, -20.075, 0.01));
    EXPECT(lower.ciHigh < 0.0 && lower.percentCiHigh < 0.0);
    // Clear regression.
    EXPECT(Compare({50.0, 51.0, 49.0}, {60.0, 61.0, 59.5}).verdict == Verdict::Higher);
    // Overlapping noise: no claim either way.
    const Comparison noise = Compare({100.0, 120.0, 90.0}, {95.0, 115.0, 105.0});
    EXPECT(noise.verdict == Verdict::NoDifference);
    EXPECT(noise.ciLow < 0.0 && noise.ciHigh > 0.0);
    // Welch-Satterthwaite df for unequal variances (4 and 100, n = 3 each) is ~2.16.
    const Comparison welch = Compare({10.0, 12.0, 14.0}, {20.0, 30.0, 40.0});
    EXPECT(Near(welch.degreesOfFreedom, 2.1597, 0.001));
    // A +18 difference is not significant with t(2) = 4.303 and se = 5.89.
    EXPECT(welch.verdict == Verdict::NoDifference);
    // One sample per side cannot be judged.
    EXPECT(Compare({1.0}, {2.0}).verdict == Verdict::NotEnoughData);
    // Identical constant samples: zero-width interval, no difference.
    EXPECT(Compare({5.0, 5.0}, {5.0, 5.0}).verdict == Verdict::NoDifference);
    EXPECT(Compare({5.0, 5.0}, {4.0, 4.0}).verdict == Verdict::Lower);
}

}  // namespace

int main() {
    TestBasics();
    TestCompare();
    if (g_failures != 0) {
        std::fprintf(stderr, "EnergyBenchStatsTests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("EnergyBenchStatsTests: all passed\n");
    return 0;
}
