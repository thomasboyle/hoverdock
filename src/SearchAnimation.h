// Spotlight reveal and hide animation math (portable; shared with DockSearch.cpp
// and SearchAnimationTests.cpp).
//
// Parameters match macOS 26 Spotlight.app's WindowAnimationCoordinator:
//   * Anchor point: layer centre (0.5, 0.5).
//   * Invocation (reveal):
//       - transform.scale.x : 1.12 -> 1.0, CASpringAnimation(perceptualDuration: 0.28, bounce: 0.32)
//       - transform.scale.y : 0.95 -> 1.0 when collapsed (bar only),
//                             1.12 -> 1.0 when vertically expanded (with results/AI),
//                             CASpringAnimation(perceptualDuration: 0.28,
//                                 bounce: collapsed ? 0.41 : 0.32)
//       - alphaValue        : 0.0  -> 1.0, CASpringAnimation(perceptualDuration: 0.28, bounce: 0.41)
//       - contentBlurRadius : 25.0 -> 0.0, CASpringAnimation(perceptualDuration: 0.51, bounce: 0.05)
//   * Dismissal (hide):
//       - transform.scale.x : 1.0  -> 1.12, CASpringAnimation(perceptualDuration: 0.45, bounce: 0.05)
//       - transform.scale.y : 1.0  -> 0.95 when collapsed, 1.0 -> 1.12 when expanded,
//                             CASpringAnimation(perceptualDuration: 0.45, bounce: 0.05)
//       - alphaValue        : 1.0  -> 0.0,  CASpringAnimation(perceptualDuration: 0.28, bounce: 0.41)
//
// Using velocity-preserving second-order springs (identical to CASpringAnimation:
// omega = 2*pi / perceptualDuration, zeta = 1 - bounce) lets mid-flight toggles
// (e.g. pressing Ctrl+Space or Escape during reveal/hide) reverse smoothly with
// continuous position and velocity instead of snapping.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace searchanim {

enum class Phase : uint8_t { Hidden, Showing, Visible, Hiding };

struct Spring {
    float value = 1.0F;
    float velocity = 0.0F;

    void Reset(float v) noexcept {
        value = v;
        velocity = 0.0F;
    }

    // Exact analytical step of x'' + 2*zeta*omega*x' + omega^2*(x - target) = 0
    // where omega = 2*pi / perceptualDuration and zeta = 1 - bounce (0 < bounce < 1),
    // matching QuartzCore's CASpringAnimation(perceptualDuration:bounce:).
    void Step(float target, float perceptualDuration, float bounce, float dt) noexcept {
        if (dt <= 0.0F) {
            return;
        }
        constexpr float kTwoPi = 6.283185307179586F;
        const float omega = kTwoPi / std::max(perceptualDuration, 0.01F);
        const float zeta = std::clamp(1.0F - bounce, 0.01F, 0.9999F);
        const float wd = omega * std::sqrt(std::max(0.0F, 1.0F - zeta * zeta));
        const float x0 = value - target;
        const float env = std::exp(-zeta * omega * dt);
        const float cosWt = std::cos(wd * dt);
        const float sinWt = std::sin(wd * dt);
        const float b = (velocity + zeta * omega * x0) / wd;
        value = target + env * (x0 * cosWt + b * sinWt);
        velocity = env * ((b * wd - zeta * omega * x0) * cosWt - (x0 * wd + zeta * omega * b) * sinWt);
    }

    [[nodiscard]] bool Settled(float target, float posEps, float velEps) const noexcept {
        return std::fabs(value - target) <= posEps && std::fabs(velocity) <= velEps;
    }
};

struct State {
    static constexpr float kWideScale = 1.12F;
    static constexpr float kCollapsedScaleY = 0.95F;
    static constexpr float kInvokeBlurPt = 25.0F;
    static constexpr float kInvokeDuration = 0.28F;
    static constexpr float kInvokeBounceTight = 0.32F;
    static constexpr float kInvokeBounceLoose = 0.41F;
    static constexpr float kInvokeBlurDuration = 0.51F;
    static constexpr float kInvokeBlurBounce = 0.05F;
    static constexpr float kDismissScaleDuration = 0.45F;
    static constexpr float kDismissScaleBounce = 0.05F;

    Phase phase = Phase::Hidden;
    bool collapsed = true;
    Spring scaleX{1.0F, 0.0F};
    Spring scaleY{1.0F, 0.0F};
    Spring alpha{0.0F, 0.0F};
    Spring blurPt{0.0F, 0.0F};

    [[nodiscard]] bool IsOpen() const noexcept { return phase == Phase::Showing || phase == Phase::Visible; }
    [[nodiscard]] bool IsAnimating() const noexcept { return phase == Phase::Showing || phase == Phase::Hiding; }
    [[nodiscard]] float ClampedAlpha() const noexcept { return std::clamp(alpha.value, 0.0F, 1.0F); }
    [[nodiscard]] float ClampedBlurPt() const noexcept { return std::max(0.0F, blurPt.value); }

    [[nodiscard]] bool IsIdentityTransform() const noexcept {
        return phase == Phase::Visible ||
            (std::fabs(scaleX.value - 1.0F) < 0.0015F && std::fabs(scaleY.value - 1.0F) < 0.0015F &&
                ClampedAlpha() >= 0.998F && ClampedBlurPt() < 0.35F);
    }

    void BeginReveal(bool isCollapsed) noexcept {
        collapsed = isCollapsed;
        if (phase == Phase::Hidden) {
            scaleX.Reset(kWideScale);
            scaleY.Reset(collapsed ? kCollapsedScaleY : kWideScale);
            alpha.Reset(0.0F);
            blurPt.Reset(kInvokeBlurPt);
        }
        phase = Phase::Showing;
    }

    void BeginHide(bool isCollapsed) noexcept {
        if (phase == Phase::Hidden) {
            return;
        }
        collapsed = isCollapsed;
        if (phase == Phase::Visible) {
            scaleX.Reset(1.0F);
            scaleY.Reset(1.0F);
            alpha.Reset(1.0F);
            blurPt.Reset(0.0F);
        }
        phase = Phase::Hiding;
    }

    void ForceHidden() noexcept {
        phase = Phase::Hidden;
        scaleX.Reset(1.0F);
        scaleY.Reset(1.0F);
        alpha.Reset(0.0F);
        blurPt.Reset(0.0F);
    }

    // Advances one frame (`dt` seconds). Returns true while still animating.
    bool Step(float dt, bool isCollapsed) noexcept {
        if (phase == Phase::Showing) {
            collapsed = isCollapsed;
        }
        dt = std::clamp(dt, 0.0F, 0.05F);
        if (phase == Phase::Showing) {
            scaleX.Step(1.0F, kInvokeDuration, kInvokeBounceTight, dt);
            scaleY.Step(1.0F, kInvokeDuration, collapsed ? kInvokeBounceLoose : kInvokeBounceTight, dt);
            alpha.Step(1.0F, kInvokeDuration, kInvokeBounceLoose, dt);
            blurPt.Step(0.0F, kInvokeBlurDuration, kInvokeBlurBounce, dt);

            if (scaleX.Settled(1.0F, 0.0012F, 0.015F) && scaleY.Settled(1.0F, 0.0012F, 0.015F) &&
                alpha.Settled(1.0F, 0.002F, 0.02F) && blurPt.Settled(0.0F, 0.25F, 1.5F)) {
                phase = Phase::Visible;
                scaleX.Reset(1.0F);
                scaleY.Reset(1.0F);
                alpha.Reset(1.0F);
                blurPt.Reset(0.0F);
                return false;
            }
            return true;
        }
        if (phase == Phase::Hiding) {
            scaleX.Step(kWideScale, kDismissScaleDuration, kDismissScaleBounce, dt);
            scaleY.Step(collapsed ? kCollapsedScaleY : kWideScale, kDismissScaleDuration, kDismissScaleBounce, dt);
            alpha.Step(0.0F, kInvokeDuration, kInvokeBounceLoose, dt);
            blurPt.Step(0.0F, kInvokeBlurDuration, kInvokeBlurBounce, dt);

            if (alpha.value <= 0.002F && (alpha.velocity <= 0.0F || alpha.Settled(0.0F, 0.002F, 0.02F))) {
                ForceHidden();
                return false;
            }
            return true;
        }
        return false;
    }
};

// Separable 3-pass box blur on premultiplied BGRA8 (~Gaussian of sigma `sigmaPx`).
inline void BlurPremulBgra(uint8_t* pixels, int width, int height, float sigmaPx,
    uint8_t* scratch) noexcept {
    if (pixels == nullptr || scratch == nullptr || width <= 1 || height <= 1 || sigmaPx < 0.45F) {
        return;
    }
    const int radius = std::clamp(static_cast<int>(std::lround(sigmaPx * 0.85F)), 1, 32);
    const int window = radius * 2 + 1;
    const uint32_t inv = ((1U << 16U) + static_cast<uint32_t>(window / 2)) / static_cast<uint32_t>(window);
    const size_t rowStride = static_cast<size_t>(width) * 4U;

    auto passAxis = [&](const uint8_t* src, uint8_t* dst, int lines, int len,
                        size_t lineStep, size_t elemStep) noexcept {
        for (int line = 0; line < lines; ++line) {
            const uint8_t* in = src + static_cast<size_t>(line) * lineStep;
            uint8_t* out = dst + static_cast<size_t>(line) * lineStep;
            uint32_t sum[4]{};
            for (int k = -radius; k <= radius; ++k) {
                const uint8_t* p = in + static_cast<size_t>(std::clamp(k, 0, len - 1)) * elemStep;
                for (int c = 0; c < 4; ++c) {
                    sum[c] += p[c];
                }
            }
            for (int i = 0; i < len; ++i) {
                uint8_t* o = out + static_cast<size_t>(i) * elemStep;
                const uint8_t* leave = in + static_cast<size_t>(std::clamp(i - radius, 0, len - 1)) * elemStep;
                const uint8_t* enter = in + static_cast<size_t>(std::clamp(i + radius + 1, 0, len - 1)) * elemStep;
                for (int c = 0; c < 4; ++c) {
                    o[c] = static_cast<uint8_t>((sum[c] * inv + 32768U) >> 16U);
                    sum[c] += enter[c] - leave[c];
                }
            }
        }
    };

    for (int pass = 0; pass < 3; ++pass) {
        passAxis(pixels, scratch, height, width, rowStride, 4U);
        passAxis(scratch, pixels, width, height, 4U, rowStride);
    }
}

// Resamples a premultiplied BGRA8 image (`srcW` x `srcH`) into a scaled canvas
// (`dstW` x `dstH`) about the centre using bilinear filtering.
inline void TransformPremulBgra(const uint8_t* src, int srcW, int srcH, float scaleX, float scaleY,
    uint8_t* dst, int dstW, int dstH) noexcept {
    if (src == nullptr || dst == nullptr || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return;
    }
    std::fill_n(dst, static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4U, static_cast<uint8_t>(0));
    if (scaleX <= 0.01F || scaleY <= 0.01F) {
        return;
    }

    const float dstCx = 0.5F * static_cast<float>(dstW);
    const float dstCy = 0.5F * static_cast<float>(dstH);
    const float srcCx = 0.5F * static_cast<float>(srcW);
    const float srcCy = 0.5F * static_cast<float>(srcH);
    const float invSx = 1.0F / scaleX;
    const float invSy = 1.0F / scaleY;

    const int xStart = std::max(0, static_cast<int>(std::floor(dstCx - srcCx * scaleX - 1.0F)));
    const int xEnd = std::min(dstW, static_cast<int>(std::ceil(dstCx + srcCx * scaleX + 1.0F)));
    const int yStart = std::max(0, static_cast<int>(std::floor(dstCy - srcCy * scaleY - 1.0F)));
    const int yEnd = std::min(dstH, static_cast<int>(std::ceil(dstCy + srcCy * scaleY + 1.0F)));
    if (xStart >= xEnd || yStart >= yEnd) {
        return;
    }

    struct Tap {
        int i0;
        int i1;
        uint32_t w0;
        uint32_t w1;
    };
    Tap xTaps[4096];
    const int spanW = std::min(xEnd - xStart, static_cast<int>(sizeof(xTaps) / sizeof(xTaps[0])));
    for (int i = 0; i < spanW; ++i) {
        const float u = (static_cast<float>(xStart + i) + 0.5F - dstCx) * invSx + srcCx - 0.5F;
        const float fu = std::floor(u);
        const int x0 = static_cast<int>(fu);
        const uint32_t wx = static_cast<uint32_t>(std::lround((u - fu) * 256.0F));
        xTaps[i] = {x0, x0 + 1, 256U - wx, wx};
    }

    const size_t srcStride = static_cast<size_t>(srcW) * 4U;
    static constexpr uint8_t kZero[4] = {0, 0, 0, 0};
    for (int dy = yStart; dy < yEnd; ++dy) {
        const float v = (static_cast<float>(dy) + 0.5F - dstCy) * invSy + srcCy - 0.5F;
        const float fv = std::floor(v);
        const int y0 = static_cast<int>(fv);
        const int y1 = y0 + 1;
        if (y1 < 0 || y0 >= srcH) {
            continue;
        }
        const uint32_t wy1 = static_cast<uint32_t>(std::lround((v - fv) * 256.0F));
        const uint32_t wy0 = 256U - wy1;
        const uint8_t* row0 = (y0 >= 0 && y0 < srcH) ? src + static_cast<size_t>(y0) * srcStride : nullptr;
        const uint8_t* row1 = (y1 >= 0 && y1 < srcH) ? src + static_cast<size_t>(y1) * srcStride : nullptr;
        uint8_t* dstRow = dst + (static_cast<size_t>(dy) * static_cast<size_t>(dstW) + static_cast<size_t>(xStart)) * 4U;

        for (int i = 0; i < spanW; ++i) {
            const Tap& tap = xTaps[i];
            if (tap.i1 < 0 || tap.i0 >= srcW) {
                continue;
            }
            const uint8_t* p00 = (row0 != nullptr && tap.i0 >= 0 && tap.i0 < srcW) ? row0 + static_cast<size_t>(tap.i0) * 4U : kZero;
            const uint8_t* p10 = (row0 != nullptr && tap.i1 >= 0 && tap.i1 < srcW) ? row0 + static_cast<size_t>(tap.i1) * 4U : kZero;
            const uint8_t* p01 = (row1 != nullptr && tap.i0 >= 0 && tap.i0 < srcW) ? row1 + static_cast<size_t>(tap.i0) * 4U : kZero;
            const uint8_t* p11 = (row1 != nullptr && tap.i1 >= 0 && tap.i1 < srcW) ? row1 + static_cast<size_t>(tap.i1) * 4U : kZero;

            const uint32_t w00 = tap.w0 * wy0;
            const uint32_t w10 = tap.w1 * wy0;
            const uint32_t w01 = tap.w0 * wy1;
            const uint32_t w11 = tap.w1 * wy1;

            uint8_t* out = dstRow + static_cast<size_t>(i) * 4U;
            for (int c = 0; c < 4; ++c) {
                out[c] = static_cast<uint8_t>((p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11 + 32768U) >> 16U);
            }
        }
    }
}

}  // namespace searchanim
