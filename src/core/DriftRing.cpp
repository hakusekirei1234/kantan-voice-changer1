// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/DriftRing.h"

#include <cmath>

namespace kvc
{

namespace
{
    /** レートが 0 で渡されたときだけ使う保険値。 */
    constexpr double kFallbackRate = 48000.0;

    /** クランプに張り付いた状態がこれだけ続いたらサンプルレート不一致を疑う。 */
    constexpr double kPinnedSeconds = 30.0;

    /** これ以上溜まったら制御では戻し切れない（±4000ppm では 20 秒以上かかる）。
        targetFill の上限が kCapacity/4 なので、健全な運転では絶対に届かない値。 */
    constexpr int kGrossOverfillExcess = DriftRing::kCapacity / 2;
}

//==============================================================================
DriftRing::DriftRing() = default;
DriftRing::~DriftRing() = default;

int DriftRing::computeTargetFill (int masterBlock_, int slaveBlock_, FillMode mode) noexcept
{
    const int m = juce::jmax (0, masterBlock_);
    const int s = juce::jmax (0, slaveBlock_);

    const int t = (mode == FillMode::lowLatency) ? (m + s + 32)
                                                 : (m + 2 * s + 64);

    return juce::jlimit (256, kCapacity / 4, t);
}

//==============================================================================
void DriftRing::prepare (int numChannelsIn,
                         double masterRate, double slaveRateIn,
                         int masterBlockIn, int slaveBlockIn,
                         FillMode mode)
{
    numChannels = juce::jlimit (1, kMaxChannels, numChannelsIn);
    slaveRate   = slaveRateIn > 0.0 ? slaveRateIn : kFallbackRate;

    const double mRate = masterRate > 0.0 ? masterRate : slaveRate;
    nominalRatio = mRate / slaveRate;

    masterBlock = juce::jmax (1, masterBlockIn);
    slaveBlock  = juce::jmax (1, slaveBlockIn);
    fillMode    = mode;

    targetFill.store (computeTargetFill (masterBlock, slaveBlock, fillMode), std::memory_order_relaxed);

    // 1 次 IIR の係数。dt = slaveBlock/slaveRate、時定数 kTauSeconds。
    smoothAlpha = juce::jlimit (1.0e-6, 1.0, static_cast<double> (slaveBlock) / (slaveRate * kTauSeconds));

    storage.setSize (numChannels, kCapacity, false, true, false);

    maxChunkOut = juce::jmax (slaveBlock, 512);

    // staging は「最大ブロック × 最大 speedRatio」ぶん必要。Lagrange は直前の
    // subSamplePos ぶん余分に食うので +2 フレームと定数マージンを足す。
    const double maxRatio = nominalRatio * (1.0 + kMaxCorrection);
    const int stagingCapacity = juce::jmax (maxChunkOut + kResamplerGuardSamples + 8,
                                            static_cast<int> (std::ceil (static_cast<double> (maxChunkOut + 2) * maxRatio))
                                                + kResamplerGuardSamples + 8);

    // 末尾 1 行は「dest に対応するチャンネルが無いとき」の捨て先。
    staging.setSize (numChannels + 1, stagingCapacity, false, true, false);

    resamplers.clear();
    for (int i = 0; i < numChannels; ++i)
        resamplers.add (new juce::Interpolators::Lagrange());

    fifo.setTotalSize (kCapacity);

    prepared = true;
    reset();
}

void DriftRing::reset()
{
    fifo.reset();
    storage.clear();
    staging.clear();

    silenceDebt = 0;
    underrunsInWindow = 0;
    windowElapsedSeconds = 0.0;
    clampedSeconds = 0.0;

    underruns.store (0, std::memory_order_relaxed);
    overruns.store  (0, std::memory_order_relaxed);
    ppmNow.store    (0.0f, std::memory_order_relaxed);
    pinned.store    (false, std::memory_order_relaxed);

    // 適応で押し上げられていた targetFill も式どおりに戻す。
    targetFill.store (computeTargetFill (masterBlock, slaveBlock, fillMode), std::memory_order_relaxed);

    for (auto* r : resamplers)
        r->reset();

    prefillSilence();
}

void DriftRing::prefillSilence()
{
    if (! prepared)
        return;

    fifo.reset();
    storage.clear();

    const int want = juce::jmin (targetFill.load (std::memory_order_relaxed), fifo.getFreeSpace());

    int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
    fifo.prepareToWrite (want, s1, b1, s2, b2);
    fifo.finishedWrite (b1 + b2);   // storage は clear 済みなので中身はすでに無音

    silenceDebt = 0;
    avgFill = static_cast<float> (b1 + b2);

    avgFillPublished.store (avgFill, std::memory_order_relaxed);
    fillPublished.store (b1 + b2, std::memory_order_relaxed);

    for (auto* r : resamplers)
        r->reset();
}

//==============================================================================
void DriftRing::write (const float* const* source, int numSourceChannels, int numSamples) noexcept
{
    if (! prepared || numSamples <= 0)
        return;

    // AbstractFifo は 1 フレーム分を常に空けておくので実効容量は kCapacity-1。
    numSamples = juce::jmin (numSamples, kCapacity - 1);

    const int freeSpace = fifo.getFreeSpace();

    if (freeSpace < numSamples)
    {
        // 最古を捨てて新しい方を残す（遅延を有界に保つ）。validStart を進めるのは
        // 本来コンシューマだけだが、オーバーラン時に限りここからも進める。両者が
        // 同時に進めても失われるのは「捨て操作 1 回」だけで、次ブロックで再試行に
        // なるだけ。validEnd を追い越すことは起こり得ない。
        fifo.finishedRead (numSamples - freeSpace);
        overruns.fetch_add (1, std::memory_order_relaxed);
    }

    int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
    fifo.prepareToWrite (numSamples, s1, b1, s2, b2);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = nullptr;

        if (source != nullptr && numSourceChannels > 0)
            src = source[juce::jmin (ch, numSourceChannels - 1)];   // 足りなければ ch0 を複製

        float* dst = storage.getWritePointer (ch);

        if (src == nullptr)
        {
            if (b1 > 0) juce::FloatVectorOperations::clear (dst + s1, b1);
            if (b2 > 0) juce::FloatVectorOperations::clear (dst + s2, b2);
        }
        else
        {
            if (b1 > 0) juce::FloatVectorOperations::copy (dst + s1, src, b1);
            if (b2 > 0) juce::FloatVectorOperations::copy (dst + s2, src + b1, b2);
        }
    }

    fifo.finishedWrite (b1 + b2);
}

//==============================================================================
void DriftRing::read (float* const* dest, int numDestChannels, int numSamples) noexcept
{
    const juce::ScopedNoDenormals noDenormals;

    if (dest == nullptr || numDestChannels <= 0 || numSamples <= 0)
        return;

    if (! prepared)
    {
        for (int ch = 0; ch < numDestChannels; ++ch)
            if (dest[ch] != nullptr)
                juce::FloatVectorOperations::clear (dest[ch], numSamples);

        return;
    }

    const int tf = targetFill.load (std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // P 制御。ブロックにつき 1 回だけ更新し、その速度比でブロック全体を作る。
    const int fillNow = silenceDebt + fifo.getNumReady();
    fillPublished.store (fillNow, std::memory_order_relaxed);

    avgFill += static_cast<float> (smoothAlpha) * (static_cast<float> (fillNow) - avgFill);
    avgFillPublished.store (avgFill, std::memory_order_relaxed);

    const double e = static_cast<double> (avgFill) - static_cast<double> (tf);
    double corr = e / (slaveRate * kSettleSeconds);

    bool atClamp = false;
    if (corr >  kMaxCorrection) { corr =  kMaxCorrection; atClamp = true; }
    if (corr < -kMaxCorrection) { corr = -kMaxCorrection; atClamp = true; }

    const double speedRatio = nominalRatio * (1.0 + corr);
    ppmNow.store (static_cast<float> (corr * 1.0e6), std::memory_order_relaxed);

    const double blockSeconds = static_cast<double> (numSamples) / slaveRate;

    clampedSeconds = atClamp ? clampedSeconds + blockSeconds : 0.0;
    pinned.store (clampedSeconds >= kPinnedSeconds, std::memory_order_relaxed);

    windowElapsedSeconds += blockSeconds;
    if (windowElapsedSeconds >= kAdaptWindowSeconds)
    {
        windowElapsedSeconds = 0.0;
        underrunsInWindow = 0;
    }

    //--------------------------------------------------------------------------
    // スレーブが一度止まって戻ってきた等でリングが埋まり切っているケース。
    // ±4000ppm では現実的な時間で戻せないので、ここで一気に切り詰める。
    if (silenceDebt == 0 && fifo.getNumReady() - tf > kGrossOverfillExcess)
    {
        fifo.finishedRead (fifo.getNumReady() - tf);
        avgFill = static_cast<float> (tf);

        for (auto* r : resamplers)
            r->reset();

        overruns.fetch_add (1, std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    const int stagingCapacity = staging.getNumSamples();
    float* scratchOut = staging.getWritePointer (numChannels);

    int  done = 0;
    bool underran = false;

    while (done < numSamples)
    {
        const int outN = juce::jmin (maxChunkOut, numSamples - done);

        // Lagrange が消費しうる入力数の保守的な上限（直前の subSamplePos ぶん +1 フレーム）。
        const int need = juce::jmin (stagingCapacity,
                                     static_cast<int> (std::ceil (static_cast<double> (outN + 1) * speedRatio)) + 2);

        const int zerosAvail = juce::jmin (silenceDebt, need);
        const int fifoWanted = need - zerosAvail;

        if (fifoWanted > fifo.getNumReady())
        {
            for (int ch = 0; ch < numDestChannels; ++ch)
                if (dest[ch] != nullptr)
                    juce::FloatVectorOperations::clear (dest[ch] + done, numSamples - done);

            underran = true;
            break;
        }

        int s1 = 0, b1 = 0, s2 = 0, b2 = 0;

        if (fifoWanted > 0)
            fifo.prepareToRead (fifoWanted, s1, b1, s2, b2);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* s = staging.getWritePointer (ch);

            if (zerosAvail > 0)
                juce::FloatVectorOperations::clear (s, zerosAvail);

            const float* src = storage.getReadPointer (ch);

            if (b1 > 0) juce::FloatVectorOperations::copy (s + zerosAvail,      src + s1, b1);
            if (b2 > 0) juce::FloatVectorOperations::copy (s + zerosAvail + b1, src + s2, b2);
        }

        const int validIn = zerosAvail + b1 + b2;
        int used = 0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* out = (ch < numDestChannels && dest[ch] != nullptr) ? dest[ch] + done
                                                                       : scratchOut;

            // 6 引数版は入力が尽きたらゼロを供給して打ち切るので、上限見積りを
            // 誤っても staging の外を読むことは無い。
            const int u = resamplers.getUnchecked (ch)->process (speedRatio,
                                                                 staging.getReadPointer (ch),
                                                                 out, outN, validIn, 0);
            if (ch == 0)
                used = u;
        }

        // 全チャンネルの消費数は同じ（速度比も subSamplePos の推移もデータ非依存）。
        const int zerosConsumed = juce::jmin (used, zerosAvail);
        const int fifoConsumed  = juce::jmax (0, used - zerosConsumed);

        silenceDebt -= zerosConsumed;

        if (fifoConsumed > 0)
            fifo.finishedRead (fifoConsumed);

        done += outN;
    }

    // リングがモノラルで dest が多チャンネルなら ch0 を複製する。
    if (dest[0] != nullptr)
        for (int ch = numChannels; ch < numDestChannels; ++ch)
            if (dest[ch] != nullptr)
                juce::FloatVectorOperations::copy (dest[ch], dest[0], numSamples);

    if (underran)
    {
        underruns.fetch_add (1, std::memory_order_relaxed);
        ++underrunsInWindow;

        if (underrunsInWindow > kAdaptUnderrunThreshold)
        {
            // 安全網。自動で下げることはしない（下げると振動する）。
            const int grown = juce::jlimit (256, kCapacity / 4,
                                            static_cast<int> (std::ceil (tf * kAdaptGrowthFactor)));
            targetFill.store (grown, std::memory_order_relaxed);

            underrunsInWindow = 0;
            windowElapsedSeconds = 0.0;
        }

        hardResync();

        // targetFill が 1 コールバック分に満たない極端な構成でも、次の read が
        // 即座にまたアンダーランして無限にリシンクを繰り返すことがないようにする。
        silenceDebt = juce::jlimit (silenceDebt, kCapacity / 2,
                                    static_cast<int> (std::ceil (static_cast<double> (numSamples + 2) * speedRatio)) + 4);
    }
}

//==============================================================================
void DriftRing::hardResync() noexcept
{
    // 段階的な追いつきはしない。動く目標を追いかけて何度もグリッチするより、
    // 1 回だけ大きく飛んで targetFill から出直す方が結果的に静か。
    const int ready = fifo.getNumReady();

    if (ready > 0)
        fifo.finishedRead (ready);

    const int tf = targetFill.load (std::memory_order_relaxed);

    silenceDebt = tf;
    avgFill = static_cast<float> (tf);
    avgFillPublished.store (avgFill, std::memory_order_relaxed);

    for (auto* r : resamplers)
        r->reset();
}

//==============================================================================
DriftRing::Diagnostics DriftRing::getDiagnostics() const noexcept
{
    Diagnostics d;

    d.ppm           = ppmNow.load (std::memory_order_relaxed);
    d.fill          = fillPublished.load (std::memory_order_relaxed);
    d.avgFill       = avgFillPublished.load (std::memory_order_relaxed);
    d.targetFill    = targetFill.load (std::memory_order_relaxed);
    d.underruns     = underruns.load (std::memory_order_relaxed);
    d.overruns      = overruns.load (std::memory_order_relaxed);
    d.pinnedAtClamp = pinned.load (std::memory_order_relaxed);

    return d;
}

} // namespace kvc
