// ActiveDockPolicy unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "ActiveDockPolicy.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                   \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (false)

void ReferenceBoxBlur2Pass(std::vector<float>& shadow, int width, int height, int blurRadius) {
    std::vector<float> blurTemp(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0F);
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float sum = 0.0F;
                int count = 0;
                const int x0 = (std::max)(0, x - blurRadius);
                const int x1 = (std::min)(width - 1, x + blurRadius);
                for (int sx = x0; sx <= x1; ++sx) {
                    sum += shadow[static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(sx)];
                    ++count;
                }
                blurTemp[static_cast<size_t>(y) * static_cast<size_t>(width) +
                    static_cast<size_t>(x)] = count > 0 ? sum / static_cast<float>(count) : 0.0F;
            }
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float sum = 0.0F;
                int count = 0;
                const int y0 = (std::max)(0, y - blurRadius);
                const int y1 = (std::min)(height - 1, y + blurRadius);
                for (int sy = y0; sy <= y1; ++sy) {
                    sum += blurTemp[static_cast<size_t>(sy) * static_cast<size_t>(width) +
                        static_cast<size_t>(x)];
                    ++count;
                }
                shadow[static_cast<size_t>(y) * static_cast<size_t>(width) +
                    static_cast<size_t>(x)] = count > 0 ? sum / static_cast<float>(count) : 0.0F;
            }
        }
    }
}

void TestMonitorHotZoneAndHookGate() {
    using active_dock_policy::FindNearestMonitorIndex;
    using active_dock_policy::IsPointInMonitorBottomHotZone;
    using active_dock_policy::IsSuppressedSelfDirt;
    using active_dock_policy::ShouldPostHookPointer;

    const std::pair<HMONITOR, RECT> monitors[2] = {
        {reinterpret_cast<HMONITOR>(1), {0, 0, 2560, 1440}},
        {reinterpret_cast<HMONITOR>(2), {2560, 180, 4480, 1260}},
    };
    EXPECT(IsPointInMonitorBottomHotZone({1200, 1435}, monitors, monitors[0].second, 8));
    EXPECT(!IsPointInMonitorBottomHotZone({1200, 1430}, monitors, monitors[0].second, 8));
    EXPECT(IsPointInMonitorBottomHotZone({3000, 1255}, monitors, monitors[0].second, 8));
    EXPECT(FindNearestMonitorIndex({4600, 500}, monitors) == 1);

    EXPECT(ShouldPostHookPointer(true, true, false, false, false, false));
    EXPECT(!ShouldPostHookPointer(true, true, true, false, false, false));
    EXPECT(!ShouldPostHookPointer(false, false, false, true, false, true));
    EXPECT(ShouldPostHookPointer(false, false, false, false, true, true));
    EXPECT(!ShouldPostHookPointer(false, false, false, false, true, false));
    EXPECT(ShouldPostHookPointer(false, false, false, false, false, false));

    const RECT dockRect{680, 1310, 1880, 1436};
    EXPECT(IsSuppressedSelfDirt({700, 1320, 1860, 1430}, dockRect, 1020ULL, 1000ULL));
    EXPECT(!IsSuppressedSelfDirt({700, 1320, 1860, 1430}, dockRect, 1200ULL, 1000ULL));
    EXPECT(!IsSuppressedSelfDirt({0, 0, 2560, 1440}, dockRect, 1010ULL, 1000ULL));
}

void TestExactSlidingHoverShadowBlur() {
    constexpr int kWidth = 260;
    constexpr int kHeight = 84;
    constexpr int kRadius = 10;
    std::vector<float> initial(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight), 0.0F);
    for (int y = 18; y < kHeight - 20; ++y) {
        for (int x = 18; x < kWidth - 18; ++x) {
            const float dx = static_cast<float>((std::min)(x - 18, (kWidth - 19) - x));
            const float dy = static_cast<float>((std::min)(y - 18, (kHeight - 21) - y));
            initial[static_cast<size_t>(y) * static_cast<size_t>(kWidth) + static_cast<size_t>(x)] =
                std::clamp((std::min)(dx, dy) / 4.0F, 0.0F, 1.0F);
        }
    }

    std::vector<float> expected = initial;
    ReferenceBoxBlur2Pass(expected, kWidth, kHeight, kRadius);

    std::vector<float> actual = initial;
    active_dock_policy::BoxBlurTruncated2Pass(actual.data(), kWidth, kHeight, kRadius);

    float maxDiff = 0.0F;
    int quantDiffs = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        maxDiff = (std::max)(maxDiff, std::fabs(actual[i] - expected[i]));
        const int qRef = static_cast<int>(std::lround(expected[i] * 0.28F * 255.0F));
        const int qAct = static_cast<int>(std::lround(actual[i] * 0.28F * 255.0F));
        if (qRef != qAct) {
            ++quantDiffs;
        }
    }
    EXPECT(maxDiff < 1.0e-6F);
    EXPECT(quantDiffs == 0);
}

}  // namespace

int main() {
    TestMonitorHotZoneAndHookGate();
    TestExactSlidingHoverShadowBlur();
    if (g_failures != 0) {
        std::fprintf(stderr, "ActiveDockPolicyTests: %d failure(s).\n", g_failures);
        return 1;
    }
    std::printf("ActiveDockPolicyTests: all passed.\n");
    return 0;
}
