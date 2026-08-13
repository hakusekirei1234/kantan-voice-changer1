// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "TestSupport.h"

#include "core/VoiceEngine.h"

#include <string>

using namespace kvctest;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr int    kBlock = 128;

    const std::vector<Formant> kVowelA { { 700.0, 90.0 }, { 1200.0, 110.0 }, { 2600.0, 160.0 } };

    std::vector<float> runEngine (const std::vector<float>& input,
                                  const kvc::VoiceParamSnapshot& params,
                                  bool noiseSuppressionInPath = true)
    {
        kvc::VoiceEngine engine;

        kvc::VoiceEngine::Config config;
        config.sampleRate = kRate;
        config.maxBlockSize = kBlock;
        config.noiseSuppressionInPath = noiseSuppressionInPath;

        engine.prepare (config);

        std::vector<float> output (input.size(), 0.0f);

        for (size_t offset = 0; offset < input.size(); offset += static_cast<size_t> (kBlock))
        {
            const int n = static_cast<int> (juce::jmin (static_cast<size_t> (kBlock),
                                                        input.size() - offset));
            engine.process (input.data() + offset, output.data() + offset, n, params);
        }

        return output;
    }

    std::string hz (double value)
    {
        char buffer[64];
        std::snprintf (buffer, sizeof (buffer), "%.1f Hz", value);
        return buffer;
    }
}

//==============================================================================

KVC_TEST (VoiceEngine, shifts_pitch_through_the_whole_chain)
{
    // シフタ単体ではなく，HPF とノイズ除去を通した実経路でピッチが動くこと。
    const auto input = makeVowel (kRate, 130.0, 3.0, kVowelA);

    kvc::VoiceParamSnapshot params;
    params.pitchSemitones = 12.0f;
    params.shifterActive = true;

    const auto output = runEngine (input, params);

    const int skip = static_cast<int> (0.8 * kRate);
    const double f0 = estimateF0 (output.data() + skip,
                                  static_cast<int> (output.size()) - skip, kRate);

    note ("130 Hz +12 st through the full chain -> " + hz (f0));

    KVC_CHECK (allFinite (output.data(), static_cast<int> (output.size())));
    KVC_CHECK_NEAR (f0, 260.0, 0.07);
}

KVC_TEST (VoiceEngine, shifts_pitch_with_noise_suppression_on)
{
    const auto input = makeVowel (kRate, 150.0, 3.0, kVowelA);

    kvc::VoiceParamSnapshot params;
    params.pitchSemitones = -7.0f;
    params.shifterActive = true;
    params.noiseSuppressionEnabled = true;

    const auto output = runEngine (input, params);

    const int skip = static_cast<int> (0.8 * kRate);
    const double f0 = estimateF0 (output.data() + skip,
                                  static_cast<int> (output.size()) - skip, kRate);

    const double expected = 150.0 * std::pow (2.0, -7.0 / 12.0);

    note ("150 Hz -7 st with denoise on -> " + hz (f0) + " (expect " + hz (expected) + ")");

    KVC_CHECK (allFinite (output.data(), static_cast<int> (output.size())));
    KVC_CHECK_NEAR (f0, expected, 0.07);
}

KVC_TEST (VoiceEngine, latency_matches_the_declared_constant)
{
    // ここが嘘をつくと，送信とモニターの遅延補正がまとめてずれる。
    for (auto quality : { kvc::NoiseQuality::lowLatency256, kvc::NoiseQuality::highQuality512 })
    {
        kvc::VoiceEngine engine;

        kvc::VoiceEngine::Config config;
        config.sampleRate = kRate;
        config.maxBlockSize = kBlock;
        config.noiseQuality = quality;

        engine.prepare (config);

        KVC_CHECK (engine.getLatencySamples() == kvc::dspLatencySamples (quality));
    }
}

KVC_TEST (VoiceEngine, latency_does_not_move_when_settings_change)
{
    kvc::VoiceEngine engine;

    kvc::VoiceEngine::Config config;
    config.sampleRate = kRate;
    config.maxBlockSize = kBlock;

    engine.prepare (config);

    const int expected = engine.getLatencySamples();

    const auto input = makeVowel (kRate, 150.0, 0.5, kVowelA);
    std::vector<float> output (input.size(), 0.0f);

    for (int semitones : { 0, 12, -12, 5 })
    {
        kvc::VoiceParamSnapshot params;
        params.pitchSemitones = static_cast<float> (semitones);
        params.formantSemitones = static_cast<float> (-semitones);
        params.shifterActive = semitones != 0;

        for (size_t offset = 0; offset < input.size(); offset += static_cast<size_t> (kBlock))
        {
            const int n = static_cast<int> (juce::jmin (static_cast<size_t> (kBlock),
                                                        input.size() - offset));
            engine.process (input.data() + offset, output.data() + offset, n, params);
        }

        KVC_CHECK (engine.getLatencySamples() == expected);
        KVC_CHECK (allFinite (output.data(), static_cast<int> (output.size())));
    }
}

KVC_TEST (VoiceEngine, bypass_leaves_the_pitch_alone)
{
    // 「声を変える」を OFF にしたら，何も動かないこと。
    const auto input = makeVowel (kRate, 160.0, 2.0, kVowelA);

    kvc::VoiceParamSnapshot params;
    params.pitchSemitones = 12.0f;      // 値は入っているが shifterActive が false
    params.shifterActive = false;

    const auto output = runEngine (input, params);

    const int skip = static_cast<int> (0.8 * kRate);
    const double f0 = estimateF0 (output.data() + skip,
                                  static_cast<int> (output.size()) - skip, kRate);

    note ("bypassed -> " + hz (f0) + " (expect 160 Hz)");

    KVC_CHECK_NEAR (f0, 160.0, 0.04);
}

KVC_TEST (VoiceEngine, handles_silence_and_extreme_input_without_nan)
{
    std::vector<float> input;

    input.insert (input.end(), static_cast<size_t> (kRate / 4), 0.0f);

    const auto loud = makeNoise (static_cast<int> (kRate / 2), 0.99f);
    input.insert (input.end(), loud.begin(), loud.end());

    input.insert (input.end(), static_cast<size_t> (kRate / 4), 0.0f);

    kvc::VoiceParamSnapshot params;
    params.pitchSemitones = 12.0f;
    params.formantSemitones = -12.0f;
    params.shifterActive = true;
    params.noiseSuppressionEnabled = true;
    params.inputGainLinear = 2.0f;

    const auto output = runEngine (input, params);

    note ("peak " + std::to_string (peak (output.data(), static_cast<int> (output.size()))));

    KVC_CHECK (allFinite (output.data(), static_cast<int> (output.size())));
    KVC_CHECK (peak (output.data(), static_cast<int> (output.size())) < 8.0);
}
