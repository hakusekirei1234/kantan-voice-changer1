// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "TestSupport.h"

#include "core/NoiseSuppressor.h"

#include <string>

using namespace kvctest;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr int    kBlock = 128;

    const std::vector<Formant> kVowelA { { 700.0, 90.0 }, { 1200.0, 110.0 }, { 2600.0, 160.0 } };

    std::vector<float> runSuppressor (const std::vector<float>& input,
                                      bool enabled, kvc::NoiseStrength strength,
                                      int fftSize = kvc::kNoiseFftLowLatency,
                                      int hopSize = kvc::kNoiseHopLowLatency)
    {
        kvc::NoiseSuppressor suppressor;
        suppressor.prepare (kRate, fftSize, hopSize, kBlock);
        suppressor.setEnabled (enabled);
        suppressor.setStrength (strength);

        std::vector<float> output (input.size(), 0.0f);

        for (size_t offset = 0; offset < input.size(); offset += static_cast<size_t> (kBlock))
        {
            const int n = static_cast<int> (juce::jmin (static_cast<size_t> (kBlock),
                                                        input.size() - offset));
            suppressor.process (input.data() + offset, output.data() + offset, n);
        }

        return output;
    }

    std::string dbText (double ratio)
    {
        char buffer[64];
        std::snprintf (buffer, sizeof (buffer), "%.2f dB", 20.0 * std::log10 (juce::jmax (1.0e-9, ratio)));
        return buffer;
    }
}

//==============================================================================

KVC_TEST (NoiseSuppressor, coefficients_match_the_reference_table)
{
    // dsp.json の参考値。ここがずれると，減衰量そのものは正しく見えても
    // 追従速度が変わって「言い始めが削れる」等の症状になる。
    const auto coef = kvc::NoiseSuppressor::deriveCoefficients (48000.0, 64);

    KVC_CHECK_NEAR (coef.dtMs, 1.33333, 0.001);
    KVC_CHECK_NEAR (coef.aS, 0.9460, 0.002);
    KVC_CHECK_NEAR (coef.aP, 0.9626, 0.002);
    KVC_CHECK_NEAR (coef.aD, 0.99749, 0.001);
    KVC_CHECK_NEAR (coef.aDfast, 0.97803, 0.002);
    KVC_CHECK_NEAR (coef.aDD, 0.98987, 0.002);
    KVC_CHECK (coef.fastFrames == 375);
    KVC_CHECK (coef.holdFrames == 188);
}

KVC_TEST (NoiseSuppressor, latency_equals_the_fft_size)
{
    for (auto quality : { kvc::NoiseQuality::lowLatency256, kvc::NoiseQuality::highQuality512 })
    {
        kvc::NoiseSuppressor suppressor;
        suppressor.prepare (kRate, kvc::fftSizeForQuality (quality), kvc::hopSizeForQuality (quality), kBlock);

        KVC_CHECK (suppressor.getLatencySamples() == kvc::fftSizeForQuality (quality));
    }
}

KVC_TEST (NoiseSuppressor, attenuates_steady_noise)
{
    // 定常ホワイトノイズだけを 3 秒。推定器が収束したあとの後半で測る。
    const auto input = makeNoise (static_cast<int> (3.0 * kRate), 0.2f);

    const auto on = runSuppressor (input, true, kvc::NoiseStrength::normal);

    const int skip = static_cast<int> (2.0 * kRate);
    const int n = static_cast<int> (input.size()) - skip;

    const double before = rms (input.data() + skip, n);
    const double after  = rms (on.data() + skip, n);

    note ("steady noise " + dbText (after / before));

    KVC_CHECK (allFinite (on.data(), static_cast<int> (on.size())));
    KVC_CHECK (after < before * 0.6);
}

KVC_TEST (NoiseSuppressor, stronger_setting_attenuates_more)
{
    const auto input = makeNoise (static_cast<int> (3.0 * kRate), 0.2f);

    const int skip = static_cast<int> (2.0 * kRate);
    const int n = static_cast<int> (input.size()) - skip;

    double previous = 1.0e18;

    for (auto strength : { kvc::NoiseStrength::weak,
                           kvc::NoiseStrength::normal,
                           kvc::NoiseStrength::strong })
    {
        const auto output = runSuppressor (input, true, strength);
        const double level = rms (output.data() + skip, n);

        note (dbText (level / rms (input.data() + skip, n)));

        KVC_CHECK (level < previous);
        previous = level;
    }
}

KVC_TEST (NoiseSuppressor, keeps_speech_when_noise_is_added)
{
    // 声 + ノイズ を通したとき，声の帯域が生き残っていること。
    // 「ノイズは消えたが声も消えた」を検出するための下限側の確認。
    auto vowel = makeVowel (kRate, 150.0, 3.0, kVowelA);
    const auto noise = makeNoise (static_cast<int> (vowel.size()), 0.05f);

    for (size_t i = 0; i < vowel.size(); ++i)
        vowel[i] = vowel[i] * 0.7f + noise[i];

    const auto output = runSuppressor (vowel, true, kvc::NoiseStrength::normal);

    const int skip = static_cast<int> (2.0 * kRate);
    const int n = static_cast<int> (vowel.size()) - skip;

    const double f0 = estimateF0 (output.data() + skip, n, kRate);
    const double level = rms (output.data() + skip, n);

    note ("surviving F0 " + std::to_string (f0) + " Hz, rms " + std::to_string (level));

    KVC_CHECK (allFinite (output.data(), static_cast<int> (output.size())));
    KVC_CHECK_NEAR (f0, 150.0, 0.06);
    KVC_CHECK (level > 0.02);
}

KVC_TEST (NoiseSuppressor, disabled_is_transparent_after_the_crossfade)
{
    // OFF のときは，STFT ぶん遅れただけの原音であること。
    const auto input = makeVowel (kRate, 150.0, 1.0, kVowelA);
    const auto output = runSuppressor (input, false, kvc::NoiseStrength::normal);

    const int latency = kvc::kNoiseFftLowLatency;
    const int start = static_cast<int> (0.3 * kRate);

    double worst = 0.0;

    for (int i = start; i < static_cast<int> (input.size()); ++i)
        worst = juce::jmax (worst, std::abs (static_cast<double> (output[static_cast<size_t> (i)])
                                                 - static_cast<double> (input[static_cast<size_t> (i - latency)])));

    note ("worst sample error " + std::to_string (worst));

    KVC_CHECK (worst < 1.0e-4);
}

KVC_TEST (NoiseSuppressor, silence_stays_silent)
{
    // 完全無音のフレームで推定器が暴れて，無音にノイズを生やさないこと。
    const std::vector<float> input (static_cast<size_t> (2.0 * kRate), 0.0f);

    const auto output = runSuppressor (input, true, kvc::NoiseStrength::strong);

    KVC_CHECK (allFinite (output.data(), static_cast<int> (output.size())));
    KVC_CHECK (peak (output.data(), static_cast<int> (output.size())) < 1.0e-6);
}
