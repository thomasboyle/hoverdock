// SearchAnimation unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "SearchAnimation.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

void TestSpringMatchesQuartzCore() {
    // Verify analytical spring step against closed-form CASpringAnimation(perceptualDuration: 0.28, bounce: 0.32)
    // for a step from 1.12 -> 1.0 over 100 ms (both single-step and 120 Hz substeps).
    searchanim::Spring single{1.12F, 0.0F};
    single.Step(1.0F, 0.28F, 0.32F, 0.10F);

    searchanim::Spring multi{1.12F, 0.0F};
    for (int i = 0; i < 12; ++i) {
        multi.Step(1.0F, 0.28F, 0.32F, 0.10F / 12.0F);
    }

    // Exact analytical steps must be time-step invariant.
    EXPECT(std::fabs(single.value - multi.value) < 1e-5F);
    EXPECT(std::fabs(single.velocity - multi.velocity) < 1e-4F);
    // At t = 0.10 s, progress is ~0.8151 -> value ~1.0222.
    EXPECT(std::fabs(single.value - 1.0222F) < 0.001F);
}

void TestCollapsedRevealAndHide() {
    searchanim::State state;
    EXPECT(!state.IsOpen());
    EXPECT(state.phase == searchanim::Phase::Hidden);

    state.BeginReveal(true /* verticallyCollapsed */);
    EXPECT(state.IsOpen());
    EXPECT(state.IsAnimating());
    EXPECT(std::fabs(state.scaleX.value - 1.12F) < 1e-5F);
    EXPECT(std::fabs(state.scaleY.value - 0.95F) < 1e-5F);
    EXPECT(std::fabs(state.alpha.value - 0.0F) < 1e-5F);
    EXPECT(std::fabs(state.blurPt.value - 25.0F) < 1e-5F);

    // Undershoot / overshoot signature from Spotlight's bounce (scaleX dips < 1, scaleY rises > 1).
    float minScaleX = state.scaleX.value;
    float maxScaleY = state.scaleY.value;
    int ticks = 0;
    while (state.Step(1.0F / 120.0F, true) && ticks < 240) {
        minScaleX = std::min(minScaleX, state.scaleX.value);
        maxScaleY = std::max(maxScaleY, state.scaleY.value);
        ++ticks;
    }
    EXPECT(state.phase == searchanim::Phase::Visible);
    EXPECT(!state.IsAnimating());
    EXPECT(minScaleX < 0.998F);
    EXPECT(maxScaleY > 1.002F);
    EXPECT(ticks > 30 && ticks < 120);

    // Dismiss from collapsed state: scaleX expands toward 1.12, scaleY compresses toward 0.95,
    // alpha drops to 0 on the fast spring (~135 ms).
    state.BeginHide(true);
    EXPECT(!state.IsOpen());
    EXPECT(state.IsAnimating());
    int hideTicks = 0;
    float lastScaleXBeforeHide = 1.0F;
    float lastScaleYBeforeHide = 1.0F;
    while (state.phase == searchanim::Phase::Hiding && hideTicks < 120) {
        lastScaleXBeforeHide = state.scaleX.value;
        lastScaleYBeforeHide = state.scaleY.value;
        state.Step(1.0F / 120.0F, true);
        ++hideTicks;
    }
    EXPECT(state.phase == searchanim::Phase::Hidden);
    EXPECT(lastScaleXBeforeHide > 1.05F);
    EXPECT(lastScaleYBeforeHide < 0.98F);
    EXPECT(hideTicks >= 12 && hideTicks <= 30);
}

void TestExpandedRevealAndMidFlightReversal() {
    searchanim::State state;
    state.BeginReveal(false /* expanded */);
    EXPECT(std::fabs(state.scaleX.value - 1.12F) < 1e-5F);
    EXPECT(std::fabs(state.scaleY.value - 1.12F) < 1e-5F);

    // Advance 60 ms into reveal, then dismiss mid-flight: position and velocity must stay continuous.
    for (int i = 0; i < 6; ++i) {
        EXPECT(state.Step(0.01F, false));
    }
    const float midX = state.scaleX.value;
    const float midAlpha = state.alpha.value;
    EXPECT(midX > 1.02F && midX < 1.10F);
    EXPECT(midAlpha > 0.35F && midAlpha < 0.85F);

    state.BeginHide(false);
    EXPECT(std::fabs(state.scaleX.value - midX) < 1e-6F);
    EXPECT(std::fabs(state.alpha.value - midAlpha) < 1e-6F);

    // Advance 30 ms into hide, then reopen mid-flight: again continuous, no snap to 0 alpha or 1.12 scale.
    for (int i = 0; i < 3; ++i) {
        state.Step(0.01F, false);
    }
    const float revAlpha = state.alpha.value;
    EXPECT(revAlpha > 0.05F);
    state.BeginReveal(false);
    EXPECT(std::fabs(state.alpha.value - revAlpha) < 1e-6F);

    int safety = 0;
    while (state.Step(1.0F / 60.0F, false) && safety < 120) {
        ++safety;
    }
    EXPECT(state.phase == searchanim::Phase::Visible);
    EXPECT(state.IsIdentityTransform());
}

void TestTransformAndBlurPremul() {
    const int srcW = 64;
    const int srcH = 32;
    std::vector<uint8_t> src(static_cast<size_t>(srcW * srcH * 4), 0);
    for (int y = 4; y < srcH - 4; ++y) {
        for (int x = 4; x < srcW - 4; ++x) {
            uint8_t* p = src.data() + static_cast<size_t>((y * srcW + x) * 4);
            p[0] = 100;
            p[1] = 150;
            p[2] = 200;
            p[3] = 220;
        }
    }

    // Centre pixel under scale about centre must preserve premultiplied colour.
    const int dstW = 80;
    const int dstH = 48;
    std::vector<uint8_t> dst(static_cast<size_t>(dstW * dstH * 4), 255);
    searchanim::TransformPremulBgra(src.data(), srcW, srcH, 1.12F, 0.95F, dst.data(), dstW, dstH);

    // Outer padding border stays exact zero.
    EXPECT(dst[0] == 0 && dst[1] == 0 && dst[2] == 0 && dst[3] == 0);

    const uint8_t* centre = dst.data() + static_cast<size_t>(((dstH / 2) * dstW + (dstW / 2)) * 4);
    EXPECT(std::abs(static_cast<int>(centre[0]) - 100) <= 1);
    EXPECT(std::abs(static_cast<int>(centre[1]) - 150) <= 1);
    EXPECT(std::abs(static_cast<int>(centre[2]) - 200) <= 1);
    EXPECT(std::abs(static_cast<int>(centre[3]) - 220) <= 1);

    // Blur spreads interior energy into the 4-px transparent border while preserving premultiplied bounds (c <= a).
    std::vector<uint8_t> blurred = src;
    std::vector<uint8_t> scratch(blurred.size(), 0);
    searchanim::BlurPremulBgra(blurred.data(), srcW, srcH, 4.0F, scratch.data());
    const uint8_t* nearEdge = blurred.data() + static_cast<size_t>(((srcH / 2) * srcW + 2) * 4);
    EXPECT(nearEdge[3] > 0 && nearEdge[3] < 220);
    for (size_t i = 0; i < blurred.size(); i += 4) {
        EXPECT(blurred[i + 0] <= blurred[i + 3] + 1);
        EXPECT(blurred[i + 1] <= blurred[i + 3] + 1);
        EXPECT(blurred[i + 2] <= blurred[i + 3] + 1);
    }
}

}  // namespace

int main() {
    TestSpringMatchesQuartzCore();
    TestCollapsedRevealAndHide();
    TestExpandedRevealAndMidFlightReversal();
    TestTransformAndBlurPremul();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d SearchAnimation test failure(s).\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("SearchAnimationTests passed.");
    return EXIT_SUCCESS;
}
