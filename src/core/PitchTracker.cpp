// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/PitchTracker.h"

#include <algorithm>
#include <cmath>

namespace kvc
{

namespace
{

/** ホップは「時間」で決まる量（128 サンプル @48 kHz = 2.667 ms）。
    レートが変わってもこの時間を保つように再導出する。 */
constexpr double kHopSeconds = static_cast<double> (PitchTracker::kHop48) / 48000.0;

/** 通過域端は解析レートに対する比で持つ。8 kHz なら 0.45*8000 = 3600 Hz となり
    dsp.json の「遮断 3.6 kHz」と一致する。 */
constexpr double kPassBandRatio = 0.45;

/** 無声がこの長さ続いたら previousLag8 を捨てる（約 300 ms 相当のフレーム数）。
    捨てないと、長い沈黙のあとに別の音高で話し始めたときオクターブガードが
    古いラグへ引き戻し、そのまま自己ラッチする。 */
constexpr int kUnvoicedResetFrames = 112;

/** Hamming 窓付き sinc の低域通過。必要タップ数は遷移域幅から逆算する
    （Hamming の経験則: 正規化遷移幅 ≈ 3.3 / N）。戻り値は実タップ数（必ず奇数）。 */
int designLowPass (float* dest, int maxTaps, double stageRate, double fPass, double fStop) noexcept
{
    const double transition = juce::jmax (1.0e-6, (fStop - fPass) / stageRate);

    int n = static_cast<int> (std::ceil (3.3 / transition));
    if ((n & 1) == 0)
        ++n;
    n = juce::jlimit (31, maxTaps, n);
    if ((n & 1) == 0)
        --n;

    const double fc = 0.5 * (fPass + fStop) / stageRate;   // -6 dB 点（正規化周波数）
    const double centre = 0.5 * (n - 1);
    double sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) - centre;
        const double sinc = std::abs (t) < 1.0e-9
                              ? 2.0 * fc
                              : std::sin (juce::MathConstants<double>::twoPi * fc * t)
                                    / (juce::MathConstants<double>::pi * t);
        const double w = 0.54 - 0.46 * std::cos (juce::MathConstants<double>::twoPi * i
                                                     / static_cast<double> (n - 1));
        const double h = sinc * w;
        dest[i] = static_cast<float> (h);
        sum += h;
    }

    juce::FloatVectorOperations::multiply (dest, static_cast<float> (1.0 / juce::jmax (1.0e-12, sum)), n);
    return n;
}

/** 二重化リング（長さ 2N）に 1 サンプル押し込み、間引き位相が合ったときだけ畳み込む。 */
inline bool stageStep (float x, const float* taps, float* line, int n,
                       int factor, int& writeIndex, int& phase, float& out) noexcept
{
    line[writeIndex] = x;
    line[writeIndex + n] = x;

    // 二重化してあるので、直近 N サンプルは常に「古い順」で連続に並ぶ。
    const float* window = line + writeIndex + 1;

    if (++writeIndex >= n)
        writeIndex = 0;

    if (++phase < factor)
        return false;

    phase = 0;

    float acc = 0.0f;
    for (int k = 0; k < n; ++k)
        acc += taps[k] * window[k];

    out = acc;
    return true;
}

inline bool isLocalMinimum (const float* d, int tau, int minLag, int maxLag) noexcept
{
    return (tau == minLag || d[tau] <= d[tau - 1])
        && (tau == maxLag || d[tau] <= d[tau + 1]);
}

/** tau が「より短い同等の谷」の整数倍かどうか（オクターブ下方チェック）。

    完全に周期的な入力では 2T, 3T ... でも波形は一致するので CMNDF はどれもほぼ 0 になる。
    これを弾かないと、話者が実際に 1 オクターブ跳ね上がったときに、前回ラグへの
    連続性優先が下位倍音を掴んだまま二度と戻らなくなる（実測で 220 Hz が 73 Hz に落ちた）。 */
bool isSubharmonic (const float* d, int tau, int minLag, int maxLag, float limit) noexcept
{
    for (int m = 2; m <= tau / minLag; ++m)
    {
        const int base = (tau + m / 2) / m;   // round(tau / m)

        for (int s = base - 1; s <= base + 1; ++s)
            if (s >= minLag && s <= maxLag && d[s] <= limit)
                return true;
    }

    return false;
}

float medianOf (const float* v, int n) noexcept
{
    float tmp[PitchTracker::kMedianLength];
    const int count = juce::jlimit (1, PitchTracker::kMedianLength, n);

    for (int i = 0; i < count; ++i)
        tmp[i] = v[i];

    for (int i = 1; i < count; ++i)
    {
        const float key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key)
        {
            tmp[j + 1] = tmp[j];
            --j;
        }
        tmp[j + 1] = key;
    }

    return tmp[count / 2];
}

} // namespace

//==============================================================================
PitchTracker::PitchTracker()
{
    reset();
}

PitchTracker::~PitchTracker() = default;

//==============================================================================
void PitchTracker::prepare (double newSampleRate)
{
    sourceRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    decimationFactor = juce::jmax (1, static_cast<int> (std::lround (sourceRate / kAnalysisRate)));
    analysisRate = sourceRate / static_cast<double> (decimationFactor);

    // 2 段に割れるときは割る。1 段目は折り返し先が 2 段目の除去域に落ちるので
    // 遷移域を広く取れて短く済み（31 タップ）、狭い遷移が要るのは最終段だけになる。
    if (decimationFactor >= 4 && (decimationFactor % 2) == 0)
    {
        numStages = 2;
        stageFactor[0] = decimationFactor / 2;
        stageFactor[1] = 2;
    }
    else
    {
        numStages = 1;
        stageFactor[0] = decimationFactor;
        stageFactor[1] = 1;
    }

    decimationState.setSize (4, 2 * kMaxFirTaps, false, true, false);
    scratch.setSize (2, kWindow8, false, true, false);

    const double fPass = kPassBandRatio * analysisRate;
    double stageRate = sourceRate;

    for (int s = 0; s < numStages; ++s)
    {
        const double outRate = stageRate / static_cast<double> (stageFactor[s]);

        // 折り返して通過域に入ってくる最下端が阻止域端。結果として -6 dB 点は
        // 常に outRate/2 になり、ハーフバンド構成と一致する。
        const double fStop = outRate - fPass;

        stageTaps[s] = designLowPass (decimationState.getWritePointer (s),
                                      kMaxFirTaps, stageRate, fPass, fStop);
        stageRate = outRate;
    }

    for (int s = numStages; s < 2; ++s)
    {
        stageTaps[s] = 0;
        stageFactor[s] = 1;
    }

    hopSource = juce::jmax (1, static_cast<int> (std::lround (sourceRate * kHopSeconds)));

    maxLag = juce::jlimit (4, kMaxLag8, static_cast<int> (std::ceil (analysisRate / 60.0)));
    minLag = juce::jlimit (2, maxLag - 1, static_cast<int> (std::floor (analysisRate / 500.0)));
    windowLength = juce::jmin (kWindow8, 2 * maxLag);

    minPeriodSource = static_cast<float> (sourceRate / 500.0);
    maxPeriodSource = static_cast<float> (sourceRate / 60.0);

    reset();
}

void PitchTracker::reset() noexcept
{
    juce::FloatVectorOperations::clear (ring8, kRing8Size);

    if (decimationState.getNumChannels() >= 4)
    {
        const int len = decimationState.getNumSamples();
        decimationState.clear (2, 0, len);
        decimationState.clear (3, 0, len);
    }

    write8 = 0;
    write48 = 0;
    nextAnalysisAt48 = hopSource;

    stageWrite[0] = stageWrite[1] = 0;
    stagePhase[0] = stagePhase[1] = 0;

    for (int i = 0; i < kMedianLength; ++i)
        lagHistory[i] = 0.0f;

    lagHistoryPos = 0;
    lagHistoryCount = 0;
    unvoicedRun = 0;
    previousLagValid = false;

    previousLag8 = static_cast<float> (analysisRate / 200.0);

    current.periodSamples48 = juce::jlimit (minPeriodSource, maxPeriodSource,
                                            static_cast<float> (sourceRate / 200.0));
    current.voicing = 0.0f;
}

//==============================================================================
void PitchTracker::pushBlock (const float* mono, int numSamples) noexcept
{
    if (mono == nullptr || numSamples <= 0 || decimationState.getNumChannels() < 4)
        return;

    const float* taps0 = decimationState.getReadPointer (0);
    const float* taps1 = decimationState.getReadPointer (1);
    float* line0 = decimationState.getWritePointer (2);
    float* line1 = decimationState.getWritePointer (3);

    for (int i = 0; i < numSamples; ++i)
    {
        float s0 = 0.0f;

        if (stageStep (mono[i], taps0, line0, stageTaps[0], stageFactor[0],
                       stageWrite[0], stagePhase[0], s0))
        {
            bool emit = true;
            float s1 = s0;

            if (numStages > 1)
                emit = stageStep (s0, taps1, line1, stageTaps[1], stageFactor[1],
                                  stageWrite[1], stagePhase[1], s1);

            if (emit)
            {
                ring8[static_cast<int> (write8 & kRing8Mask)] = s1;
                ++write8;
            }
        }

        ++write48;

        if (write48 >= nextAnalysisAt48)
        {
            nextAnalysisAt48 = write48 + hopSource;
            analyseFrame();
        }
    }
}

//==============================================================================
void PitchTracker::analyseFrame() noexcept
{
    if (write8 < windowLength)
        return;

    float* x = scratch.getWritePointer (0);
    float* d = scratch.getWritePointer (1);

    const int64_t start = write8 - windowLength;
    float energy = 0.0f;

    for (int i = 0; i < windowLength; ++i)
    {
        const float s = ring8[static_cast<int> ((start + i) & kRing8Mask)];
        x[i] = s;
        energy += s * s;
    }

    // 無音〜ほぼ無音に周期性を読み取らせない（-90 dBFS 相当で足切り）。
    if (energy < 1.0e-9f * static_cast<float> (windowLength))
    {
        current.voicing = 0.0f;
        if (++unvoicedRun >= kUnvoicedResetFrames)
        {
            previousLagValid = false;
            lagHistoryCount = 0;
        }
        return;
    }

    // --- YIN 差分関数。積分長は 1 最大周期、データ span は 2 最大周期。 ---
    const int integration = maxLag;

    d[0] = 0.0f;

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        const float* a = x;
        const float* b = x + tau;
        float acc = 0.0f;

        for (int j = 0; j < integration; ++j)
        {
            const float diff = a[j] - b[j];
            acc += diff * diff;
        }

        d[tau] = acc;
    }

    // --- 累積平均正規化（CMNDF）。in-place で安全: d[tau] は加算後に上書きする。 ---
    float running = 0.0f;
    d[0] = 1.0f;

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        running += d[tau];
        d[tau] = running > 1.0e-20f ? d[tau] * static_cast<float> (tau) / running : 1.0f;
    }

    // --- 全体最小 ---
    int bestTau = minLag;
    float dMin = d[minLag];

    for (int tau = minLag + 1; tau <= maxLag; ++tau)
    {
        if (d[tau] < dMin)
        {
            dMin = d[tau];
            bestTau = tau;
        }
    }

    // --- YIN の絶対閾値: 閾値を下回る「最初の」谷。倍周期の選択を防ぐ第一の壁。 ---
    int absTau = -1;

    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (d[tau] < kVoicedThreshold)
        {
            while (tau + 1 <= maxLag && d[tau + 1] < d[tau])
                ++tau;

            absTau = tau;
            break;
        }
    }

    int chosen = absTau >= minLag ? absTau : bestTau;

    // 同点判定の基準値は「全体最小」ではなく絶対閾値で選んだ谷の値に置く。
    // CMNDF は累積平均で割るのでラグが延びるほど単調に小さくなり、完全に周期的な
    // 入力では全体最小が基本周期ではなく最長の整数倍に付く（実測: 220 Hz で
    // d[36]=0.0097, d[73]=0.0055, d[109]=0.0006）。dMin を基準にすると候補集合が
    // 下位倍音しか含まなくなり、ガードが構造的にオクターブ下へ引きずり込む。
    const float reference = absTau >= minLag ? d[absTau] : dMin;
    const float limit = kOctaveGuardRatio * reference + 1.0e-4f;

    if (isSubharmonic (d, chosen, minLag, maxLag, limit))
    {
        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            if (d[tau] <= limit
                && isLocalMinimum (d, tau, minLag, maxLag)
                && ! isSubharmonic (d, tau, minLag, maxLag, limit))
            {
                chosen = tau;
                break;
            }
        }
    }

    // --- オクターブガード: 同等の谷のうち前回ラグに最も近いもの。 ---
    if (previousLagValid)
    {
        float bestDistance = std::abs (static_cast<float> (chosen) - previousLag8);

        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            if (d[tau] > limit || ! isLocalMinimum (d, tau, minLag, maxLag))
                continue;

            if (isSubharmonic (d, tau, minLag, maxLag, limit))
                continue;

            const float distance = std::abs (static_cast<float> (tau) - previousLag8);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                chosen = tau;
            }
        }
    }

    // --- 放物線補間 ---
    float refinedLag = static_cast<float> (chosen);
    float dAtMin = d[chosen];

    if (chosen > minLag && chosen < maxLag)
    {
        const float y0 = d[chosen - 1];
        const float y1 = d[chosen];
        const float y2 = d[chosen + 1];
        const float denom = y0 - 2.0f * y1 + y2;

        if (denom > 1.0e-12f)
        {
            const float delta = juce::jlimit (-0.5f, 0.5f, 0.5f * (y0 - y2) / denom);
            refinedLag = static_cast<float> (chosen) + delta;
            dAtMin = juce::jmax (0.0f, y1 - 0.25f * (y0 - y2) * delta);
        }
    }

    // --- 連続値の有声確率。ハード切替はしない。 ---
    const float voicing = juce::jlimit (0.0f, 1.0f,
                                        (kUnvoicedThreshold - dAtMin)
                                            / (kUnvoicedThreshold - kVoicedThreshold));
    current.voicing = voicing;

    // v < 0.4 では T0 を更新せず前回値を保持する。雑音を追うと PSOLA が一発で壊れる。
    if (voicing < kHoldVoicingBelow)
    {
        if (++unvoicedRun >= kUnvoicedResetFrames)
        {
            previousLagValid = false;
            lagHistoryCount = 0;
        }
        return;
    }

    unvoicedRun = 0;

    lagHistory[lagHistoryPos] = refinedLag;
    lagHistoryPos = (lagHistoryPos + 1) % kMedianLength;
    if (lagHistoryCount < kMedianLength)
        ++lagHistoryCount;

    const float acceptedLag = medianOf (lagHistory, lagHistoryCount);

    previousLag8 = acceptedLag;
    previousLagValid = true;

    current.periodSamples48 = juce::jlimit (minPeriodSource, maxPeriodSource,
                                            acceptedLag * static_cast<float> (decimationFactor));
}

} // namespace kvc
