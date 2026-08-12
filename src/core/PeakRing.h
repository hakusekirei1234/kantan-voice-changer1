// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace kvc
{

/** 波形表示 1 列ぶんの min/max。 */
struct Peak
{
    float lo = 0.0f;
    float hi = 0.0f;
};

//==============================================================================
/** 波形表示とレベルメーターのための固定長リング。ヘッダオンリー。

    単一生産者（入力デバイスのコールバック）・単一消費者（GUI）。
    消費者は書込カーソルの「後ろ」しか読まないので AbstractFifo は要らない。
    書込カーソルを release で公開し、GUI は acquire で読む。

    スレッド契約:
      pushSamples / resetAccumulator                     ... オーディオスレッド専用
      setDecimation / getDecimation                      ... どちらからでも可
      getWritePos / peakAt / getMeterPeak / getClipCount ... GUI 専用（読むだけ）
      reset                                              ... デバイス停止中のみ
*/
class PeakRing
{
public:
    static constexpr int kRingSize = 8192;          // 64 KB。decim=32 なら約 5.4 秒ぶん。
    static constexpr int kRingMask = kRingSize - 1;

    PeakRing() = default;

    //==========================================================================
    // --- オーディオスレッド ---

    /** モノラルのサンプル列を流し込む。min/max デシメーションのみ、確保なし。
        レベルメーター用のブロックピークも同時に更新する。 */
    void pushSamples (const float* samples, int numSamples) noexcept
    {
        const int decim = std::max (1, decimation.load (std::memory_order_relaxed));
        float blockPeak = 0.0f;
        bool  clipped = false;

        for (int i = 0; i < numSamples; ++i)
        {
            const float s = samples[i];
            accLo = std::min (accLo, s);
            accHi = std::max (accHi, s);

            const float a = std::fabs (s);
            if (a > blockPeak) blockPeak = a;
            if (a >= 0.99f)    clipped = true;

            if (++accN >= decim)
            {
                const uint32_t w = writePos.load (std::memory_order_relaxed);
                ring[w & kRingMask] = { accLo, accHi };
                writePos.store (w + 1, std::memory_order_release);   // 中身を書いてから公開
                accLo = 1.0e9f; accHi = -1.0e9f; accN = 0;
            }
        }

        meterPeak.store (blockPeak, std::memory_order_relaxed);
        if (clipped)
            clipCount.fetch_add (1, std::memory_order_relaxed);
    }

    void resetAccumulator() noexcept
    {
        accLo = 1.0e9f; accHi = -1.0e9f; accN = 0;
    }

    //==========================================================================
    // --- どちらのスレッドからでも ---

    /** 1 ピーク当たりのサンプル数。GUI が resized() で再計算して書き込む。
        リングは固定長なので再確保は決して起きない。 */
    void setDecimation (int samplesPerPeak) noexcept
    {
        decimation.store (std::max (1, std::min (512, samplesPerPeak)), std::memory_order_relaxed);
    }

    int getDecimation() const noexcept { return decimation.load (std::memory_order_relaxed); }

    /** GUI 側の推奨式: sampleRate * windowSeconds / (1.5 * widthPx) を 8..512 に丸める。 */
    static int suggestDecimation (double sampleRate, double windowSeconds, int widthPx) noexcept
    {
        if (widthPx <= 0) return 32;
        const double d = sampleRate * windowSeconds / (1.5 * static_cast<double> (widthPx));
        return std::max (8, std::min (512, static_cast<int> (d + 0.5)));
    }

    //==========================================================================
    // --- GUI スレッド ---

    uint32_t getWritePos() const noexcept { return writePos.load (std::memory_order_acquire); }

    /** index は絶対カーソル値。内部でマスクする。 */
    const Peak& peakAt (uint32_t index) const noexcept { return ring[index & kRingMask]; }

    float    getMeterPeak() const noexcept { return meterPeak.load (std::memory_order_relaxed); }
    uint32_t getClipCount() const noexcept { return clipCount.load (std::memory_order_relaxed); }

    //==========================================================================
    /** デバイス停止中にのみ呼ぶこと。 */
    void reset() noexcept
    {
        for (auto& p : ring) p = {};
        writePos.store (0, std::memory_order_release);
        meterPeak.store (0.0f, std::memory_order_relaxed);
        clipCount.store (0, std::memory_order_relaxed);
        resetAccumulator();
    }

private:
    Peak ring[kRingSize] {};
    std::atomic<uint32_t> writePos { 0 };
    std::atomic<int>      decimation { 32 };
    std::atomic<float>    meterPeak { 0.0f };
    std::atomic<uint32_t> clipCount { 0 };

    // オーディオスレッド専有。atomic である必要はない。
    float accLo = 1.0e9f;
    float accHi = -1.0e9f;
    int   accN = 0;
};

} // namespace kvc
