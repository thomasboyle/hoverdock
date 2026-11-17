// AudioPolicy unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#define DOCK_AUDIO_POLICY_NO_WIN32
#include "AudioPolicy.h"

#include <cmath>
#include <cstdio>
#include <string>
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

void TestBluetoothFilterClassification() {
    using audio_policy::BtAudioProfile;
    using audio_policy::ClassifyBtAudioFilter;
    using audio_policy::FilterMatchesBtAddress;
    using audio_policy::IsBluetoothConnectorId;

    const std::wstring a2dpFilter =
        L"{2}.\\\\?\\btha2dp#service_{0000110b-0000-1000-8000-00805f9b34fb}_dev&a4c1389012ef#8&12ab&0#{6994ad04}";
    const std::wstring hfpFilter =
        L"{2}.\\\\?\\bthhfenum#bthhfpaudio#9&23cd45&0&97#{65e8773d}_a4c1389012ef";
    const std::wstring hfpRenderBthenum =
        L"{2}.\\\\?\\bthenum#{0000111e-0000-1000-8000-00805f9b34fb}_localmfg&0002#7&a4c1389012ef";
    const std::wstring usbDac =
        L"{2}.\\\\?\\usb#vid_05ac&pid_110a&mi_00#6&1f2e3d4c&0&0000#{6994ad04}";
    const std::wstring hdAudio =
        L"{2}.\\\\?\\hdaudio#func_01&ven_10ec&dev_0256#5&998877&0&0001#{6994ad04}";

    EXPECT(IsBluetoothConnectorId(a2dpFilter));
    EXPECT(IsBluetoothConnectorId(hfpFilter));
    EXPECT(IsBluetoothConnectorId(hfpRenderBthenum));
    EXPECT(!IsBluetoothConnectorId(usbDac));
    EXPECT(!IsBluetoothConnectorId(hdAudio));
    EXPECT(!IsBluetoothConnectorId(L""));

    // Stereo A2DP render vs Hands-Free render/capture separation.
    EXPECT(ClassifyBtAudioFilter(a2dpFilter, true) == BtAudioProfile::A2dpStereo);
    EXPECT(ClassifyBtAudioFilter(a2dpFilter, false) == BtAudioProfile::HandsFree);
    EXPECT(ClassifyBtAudioFilter(hfpFilter, true) == BtAudioProfile::HandsFree);
    EXPECT(ClassifyBtAudioFilter(hfpFilter, false) == BtAudioProfile::HandsFree);
    EXPECT(ClassifyBtAudioFilter(hfpRenderBthenum, true) == BtAudioProfile::HandsFree);
    EXPECT(ClassifyBtAudioFilter(usbDac, true) == BtAudioProfile::None);

    constexpr uint64_t kAirPodsAddr = 0x0000A4C1389012EFULL;
    constexpr uint64_t kOtherAddr = 0x0000112233445566ULL;
    EXPECT(FilterMatchesBtAddress(a2dpFilter, kAirPodsAddr));
    EXPECT(FilterMatchesBtAddress(hfpFilter, kAirPodsAddr));
    EXPECT(FilterMatchesBtAddress(L"BTHLEDevice_A4:C1:38:90:12:EF", kAirPodsAddr));
    EXPECT(!FilterMatchesBtAddress(a2dpFilter, kOtherAddr));
    EXPECT(!FilterMatchesBtAddress(a2dpFilter, 0));
}

void TestConnectAndDisconnectFilterSelection() {
    const std::vector<std::wstring> a2dp{L"filter_a2dp"};
    const std::vector<std::wstring> hfp{L"filter_hfp"};
    const std::vector<std::wstring> empty;

    // Stereo headsets must page ONLY A2DP on Connect even when HFP is enabled,
    // preventing sequential KsProperty stalls and RFCOMM/AVDTP contention.
    const auto stereoWithHfp = audio_policy::SelectConnectFilters(a2dp, hfp, true);
    EXPECT(stereoWithHfp.size() == 1 && stereoWithHfp[0] == L"filter_a2dp");

    const auto stereoNoHfp = audio_policy::SelectConnectFilters(a2dp, hfp, false);
    EXPECT(stereoNoHfp.size() == 1 && stereoNoHfp[0] == L"filter_a2dp");

    // Voice-only earpiece (no A2DP filter) falls back to HFP only when HFP is allowed.
    const auto voiceAllowed = audio_policy::SelectConnectFilters(empty, hfp, true);
    EXPECT(voiceAllowed.size() == 1 && voiceAllowed[0] == L"filter_hfp");
    EXPECT(audio_policy::SelectConnectFilters(empty, hfp, false).empty());

    // Disconnect closes every profile so HFP cannot hold the ACL link up.
    const auto disconnect = audio_policy::SelectDisconnectFilters(a2dp, hfp);
    EXPECT(disconnect.size() == 2);
    EXPECT(disconnect[0] == L"filter_a2dp" && disconnect[1] == L"filter_hfp");

    // PostConnect attaches HFP only after stereo is active and HFP is still unplugged.
    EXPECT(audio_policy::ShouldReconnectHfpPostConnect(true, true, false, true));
    EXPECT(!audio_policy::ShouldReconnectHfpPostConnect(false, true, false, true));
    EXPECT(!audio_policy::ShouldReconnectHfpPostConnect(true, false, false, true));
    EXPECT(!audio_policy::ShouldReconnectHfpPostConnect(true, true, true, true));
    EXPECT(!audio_policy::ShouldReconnectHfpPostConnect(true, true, false, false));
}

void TestCaptureStreamScoGuard() {
    using audio_policy::ShouldOpenCaptureStream;

    // Bluetooth capture endpoints must NEVER be opened passively on Home or on
    // the Bluetooth page (doing so switches AirPods from A2DP stereo into HFP SCO).
    EXPECT(!ShouldOpenCaptureStream(true, true, false, true, true));
    EXPECT(!ShouldOpenCaptureStream(true, false, false, true, true));
    EXPECT(!ShouldOpenCaptureStream(false, false, true, true, true));

    // Explicit Microphone test page opens BT mic only if Hands-Free is enabled.
    EXPECT(ShouldOpenCaptureStream(true, false, true, true, true));
    EXPECT(!ShouldOpenCaptureStream(true, false, true, true, false));

    // Wired / built-in mics work on Home and Microphone, but stay closed elsewhere.
    EXPECT(ShouldOpenCaptureStream(true, true, false, false, false));
    EXPECT(ShouldOpenCaptureStream(true, false, true, false, false));
    EXPECT(!ShouldOpenCaptureStream(true, false, false, false, true));
    EXPECT(!ShouldOpenCaptureStream(false, true, false, false, true));
}

void TestAirPodsBalanceDriftDetectionAndRepair() {
    using audio_policy::ComputeChannelVolumeTargets;
    using audio_policy::NeedsBluetoothBalanceRepair;
    using audio_policy::SavedChannelBalance;

    // Balanced AirPods within float noise: no repair needed.
    const float balanced[2] = {0.64F, 0.6405F};
    float target = 0.0F;
    EXPECT(!NeedsBluetoothBalanceRepair(balanced, 2, &target));

    // Slight 1-AVRCP-step skew (~0.8%) or larger single-earbud drift is caught
    // immediately before proportional scaling can ratchet one ear quieter.
    const float oneStepSkew[2] = {0.50F, 0.492F};
    EXPECT(NeedsBluetoothBalanceRepair(oneStepSkew, 2, &target));
    EXPECT(std::fabs(target - 0.50F) < 1e-6F);

    const float driftedAirPods[2] = {0.38F, 0.72F};
    EXPECT(NeedsBluetoothBalanceRepair(driftedAirPods, 2, &target));
    EXPECT(std::fabs(target - 0.72F) < 1e-6F);

    // Simulate 50 slider sweeps down to near-zero (2%) and back up on Bluetooth:
    // both ears must stay identically equal at every step without drift.
    SavedChannelBalance btSaved{};
    float currentBt[2] = {0.65F, 0.41F};
    float outBt[2]{};
    const float sweep[] = {0.30F, 0.02F, 0.00F, 0.03F, 0.55F, 0.85F};
    for (float master : sweep) {
        ComputeChannelVolumeTargets(master, true, currentBt, 2, btSaved, outBt);
        EXPECT(std::fabs(outBt[0] - master) < 1e-6F);
        EXPECT(std::fabs(outBt[1] - master) < 1e-6F);
        // Even if hardware step quantization perturbs one channel slightly:
        currentBt[0] = outBt[0];
        currentBt[1] = std::max(0.0F, outBt[1] - 0.008F);
    }
}

void TestWiredCustomBalancePreservedAcrossZeroSweep() {
    using audio_policy::ComputeChannelVolumeTargets;
    using audio_policy::SavedChannelBalance;

    // Intentional 80% Left / 40% Right (2:1) balance on wired desktop speakers:
    // sweeping down through 1% -> 0% -> back to 60% preserves exact 2:1 ratio.
    SavedChannelBalance wiredSaved{};
    float current[2] = {0.80F, 0.40F};
    float out[2]{};

    ComputeChannelVolumeTargets(0.02F, false, current, 2, wiredSaved, out);
    EXPECT(std::fabs(out[0] - 0.02F) < 1e-5F);
    EXPECT(std::fabs(out[1] - 0.01F) < 1e-5F);

    // Simulate coarse driver quantization near 0% (clamping right channel to 0):
    current[0] = 0.02F;
    current[1] = 0.00F;
    ComputeChannelVolumeTargets(0.00F, false, current, 2, wiredSaved, out);
    current[0] = 0.00F;
    current[1] = 0.00F;

    // Raising back to 0.60 restores 0.60 Left / 0.30 Right instead of 0.60 / 0.00.
    ComputeChannelVolumeTargets(0.60F, false, current, 2, wiredSaved, out);
    EXPECT(std::fabs(out[0] - 0.60F) < 1e-5F);
    EXPECT(std::fabs(out[1] - 0.30F) < 1e-5F);

    // Balanced wired speakers (within 1.5% step noise) stay locked equal.
    SavedChannelBalance centeredSaved{};
    const float nearlyEqual[2] = {0.50F, 0.492F};
    ComputeChannelVolumeTargets(0.75F, false, nearlyEqual, 2, centeredSaved, out);
    EXPECT(std::fabs(out[0] - 0.75F) < 1e-6F);
    EXPECT(std::fabs(out[1] - 0.75F) < 1e-6F);
}

}  // namespace

int main() {
    TestBluetoothFilterClassification();
    TestConnectAndDisconnectFilterSelection();
    TestCaptureStreamScoGuard();
    TestAirPodsBalanceDriftDetectionAndRepair();
    TestWiredCustomBalancePreservedAcrossZeroSweep();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d AudioPolicy test assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("AudioPolicyTests: all passed.\n");
    return 0;
}
