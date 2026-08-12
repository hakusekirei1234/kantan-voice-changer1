// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>

namespace kvc
{

//==============================================================================
/** GUI が FFT をかけるための「生サンプル」のリング。単一生産者・単一消費者。

    PeakRing は表示用に min/max へ間引いた値しか持たないので、周波数解析には使えない。
    こちらは間引かない生の波形をそのまま置く。

    オーディオスレッドが書き、GUI は書込カーソルの手前だけを読む。
    ロックもアロケーションも無い。読んでいる最中に上書きされてもスペクトルが
    1 フレーム乱れるだけなので、そこは許容する（音には一切影響しない）。

    スレッド契約:
      push       ... オーディオスレッド専用
      readLatest ... メッセージスレッド専用
*/
class RawRing
{
public:
    /** 2 のべき乗。48 kHz で約 341 ms。FFT 4096 点を余裕をもって賄える。 */
    static constexpr int kSize = 16384;
    static constexpr int kMask = kSize - 1;

    void push (const float* samples, int numSamples) noexcept
    {
        if (samples == nullptr || numSamples <= 0)
            return;

        const auto w = writePos.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
            buffer[(w + i) & kMask] = samples[i];

        writePos.store (w + numSamples, std::memory_order_release);
    }

    /** 直近 numSamples を古い順に dest へ写す。numSamples <= kSize。 */
    void readLatest (float* dest, int numSamples) const noexcept
    {
        if (dest == nullptr || numSamples <= 0 || numSamples > kSize)
            return;

        const auto w = writePos.load (std::memory_order_acquire);
        const auto start = w - numSamples;

        for (int i = 0; i < numSamples; ++i)
            dest[i] = buffer[(start + i) & kMask];
    }

    std::int64_t getWritePos() const noexcept { return writePos.load (std::memory_order_acquire); }

private:
    float buffer[kSize] {};
    std::atomic<std::int64_t> writePos { 0 };
};

} // namespace kvc
