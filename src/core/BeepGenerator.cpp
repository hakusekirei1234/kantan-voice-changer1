// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/BeepGenerator.h"

#include <cmath>

namespace kvc
{

BeepGenerator::BeepGenerator() = default;
BeepGenerator::~BeepGenerator() = default;

//==============================================================================
void BeepGenerator::prepare (double sampleRate)
{
    if (sampleRate <= 0.0)
        sampleRate = 48000.0;

    const int len  = juce::jmax (16, juce::roundToInt (kDurationSeconds * sampleRate));
    const int ramp = juce::jlimit (1, len / 2, juce::roundToInt (kRampSeconds * sampleRate));

    waveforms.setSize (2, len, false, true, false);

    const double freqs[2] = { kFreqMuteOn, kFreqMuteOff };

    for (int k = 0; k < 2; ++k)
    {
        float* w = waveforms.getWritePointer (k);
        const double inc = juce::MathConstants<double>::twoPi * freqs[k] / sampleRate;

        for (int i = 0; i < len; ++i)
        {
            // レイズドコサインで両端を 0 から立ち上げる（クリック防止）。
            double env = 1.0;

            if (i < ramp)
                env = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * static_cast<double> (i) / static_cast<double> (ramp));
            else if (i >= len - ramp)
                env = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * static_cast<double> (len - 1 - i) / static_cast<double> (ramp));

            w[i] = static_cast<float> (env * std::sin (inc * static_cast<double> (i)));
        }
    }

    lengthSamples = len;
    prepared = true;

    resetPlayback();
}

//==============================================================================
void BeepGenerator::setVolume (float linearGain) noexcept
{
    volume.store (juce::jlimit (0.0f, 1.0f, linearGain), std::memory_order_relaxed);
}

void BeepGenerator::setEnabled (bool shouldBeEnabled) noexcept
{
    enabled.store (shouldBeEnabled, std::memory_order_relaxed);
}

void BeepGenerator::trigger (Kind kind) noexcept
{
    // 種別を先に置いてからシーケンス番号を release で公開する。オーディオ側は
    // 番号の変化だけを見るので、連打しても「鳴らし損ね」は起きない。
    kindRequested.store (static_cast<int> (kind), std::memory_order_relaxed);
    seq.fetch_add (1, std::memory_order_release);
}

void BeepGenerator::resetPlayback() noexcept
{
    playPos = -1;
    playIndex = 0;
    lastSeqSeen = seq.load (std::memory_order_acquire);
}

//==============================================================================
void BeepGenerator::addToMonitor (float* const* dest, int numDestChannels, int numSamples) noexcept
{
    if (! prepared || dest == nullptr || numDestChannels <= 0 || numSamples <= 0)
        return;

    const uint32_t s = seq.load (std::memory_order_acquire);

    if (s != lastSeqSeen)
    {
        lastSeqSeen = s;

        if (enabled.load (std::memory_order_relaxed))
        {
            playIndex = (kindRequested.load (std::memory_order_relaxed) == static_cast<int> (Kind::muteOff)) ? 1 : 0;
            playPos = 0;
        }
    }

    if (playPos < 0)
        return;

    const int n = juce::jmin (numSamples, lengthSamples - playPos);

    if (n <= 0)
    {
        playPos = -1;
        return;
    }

    const float g = volume.load (std::memory_order_relaxed);

    if (g > 0.0f)
    {
        const float* src = waveforms.getReadPointer (playIndex) + playPos;

        for (int ch = 0; ch < numDestChannels; ++ch)
            if (dest[ch] != nullptr)
                juce::FloatVectorOperations::addWithMultiply (dest[ch], src, g, n);
    }

    playPos += n;

    if (playPos >= lengthSamples)
        playPos = -1;
}

} // namespace kvc
