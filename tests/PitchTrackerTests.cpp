// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "TestSupport.h"

#include "core/PitchTracker.h"

#include <string>

using namespace kvctest;

namespace
{
    constexpr double kRate = 48000.0;

    const std::vector<Formant> kVowelA { { 700.0, 90.0 }, { 1200.0, 110.0 }, { 2600.0, 160.0 } };

    kvc::PitchTracker::Result trackTail (const std::vector<float>& input, int blockSize = 128)
    {
        kvc::PitchTracker tracker;
        tracker.prepare (kRate);

        for (size_t offset = 0; offset < input.size(); offset += static_cast<size_t> (blockSize))
        {
            const int n = static_cast<int> (juce::jmin (static_cast<size_t> (blockSize),
                                                        input.size() - offset));
            tracker.pushBlock (input.data() + offset, n);
        }

        return tracker.getCurrent();
    }

    std::string hz (double value)
    {
        char buffer[64];
        std::snprintf (buffer, sizeof (buffer), "%.1f Hz", value);
        return buffer;
    }
}

//==============================================================================

KVC_TEST (PitchTracker, finds_f0_across_the_speaking_range)
{
    // オクターブ誤りは PSOLA の音質を一発で壊す。低い男声から高い女声まで舐める。
    for (double f0 : { 70.0, 100.0, 130.0, 180.0, 240.0, 330.0 })
    {
        const auto input = makeVowel (kRate, f0, 1.5, kVowelA);
        const auto result = trackTail (input);

        const double measured = kRate / static_cast<double> (result.periodSamples48);

        note (hz (f0) + " -> " + hz (measured) + ", voicing "
                  + std::to_string (result.voicing));

        KVC_CHECK_NEAR (measured, f0, 0.05);
        KVC_CHECK (result.voicing > 0.5f);
    }
}

KVC_TEST (PitchTracker, reports_noise_as_unvoiced)
{
    const auto input = makeNoise (static_cast<int> (kRate), 0.3f);
    const auto result = trackTail (input);

    note ("voicing on white noise = " + std::to_string (result.voicing));

    KVC_CHECK (result.voicing < 0.5f);
}

KVC_TEST (PitchTracker, period_is_always_within_its_clamp)
{
    // 出力が範囲外に出ると，シフタ側のグレイン長計算がそのまま壊れる。
    std::vector<float> input;

    const auto noise = makeNoise (static_cast<int> (kRate / 2), 0.9f);
    input.insert (input.end(), noise.begin(), noise.end());

    input.insert (input.end(), static_cast<size_t> (kRate / 4), 0.0f);

    const auto vowel = makeVowel (kRate, 90.0, 0.5, kVowelA);
    input.insert (input.end(), vowel.begin(), vowel.end());

    kvc::PitchTracker tracker;
    tracker.prepare (kRate);

    for (size_t offset = 0; offset < input.size(); offset += 64)
    {
        const int n = static_cast<int> (juce::jmin (size_t (64), input.size() - offset));
        tracker.pushBlock (input.data() + offset, n);

        const auto result = tracker.getCurrent();

        KVC_CHECK (result.periodSamples48 >= kvc::PitchTracker::kMinPeriod48);
        KVC_CHECK (result.periodSamples48 <= kvc::PitchTracker::kMaxPeriod48);
        KVC_CHECK (result.voicing >= 0.0f && result.voicing <= 1.0f);
    }
}

KVC_TEST (PitchTracker, decimated_ring_position_matches_the_input_position)
{
    // シフタは 8 kHz リングと 48 kHz 位置を相互に変換する。
    // この対応が崩れると相関ロックが別の場所を掴む。
    kvc::PitchTracker tracker;
    tracker.prepare (kRate);

    const auto input = makeVowel (kRate, 150.0, 0.5, kVowelA);

    for (size_t offset = 0; offset < input.size(); offset += 97)
    {
        const int n = static_cast<int> (juce::jmin (size_t (97), input.size() - offset));
        tracker.pushBlock (input.data() + offset, n);

        KVC_CHECK (tracker.getDecimatedWritePos()
                       == tracker.getInputWritePos() / tracker.getDecimationFactor());
    }
}
