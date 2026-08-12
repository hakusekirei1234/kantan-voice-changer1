// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>

namespace kvc
{

namespace
{

//==============================================================================
// state / scratch の行割り当て。ヘッダのコメントと同じ並び。
enum StateRow
{
    rowS = 0,     // 平滑パワー S[k]
    rowSprev,     // 1 フレーム前の S[k]（Doblinger が要る）
    rowSmin,      // 連続最小値追跡
    rowSpp,       // 音声存在確率 p[k]
    rowNoise,     // 雑音パワー N[k]
    rowGprev,     // 1 フレーム前の生ゲイン
    rowDd,        // 決定指向の記憶項
    rowBeta,      // 帯域別の過剰推定係数
    rowDelta,     // MCRA のしきい値
    numStateRows
};

enum ScratchRow
{
    rowPow = 0,   // 生パワー
    rowSf,        // 周波数平滑したパワー
    rowNb,        // 過剰推定後の雑音パワー
    rowGain,      // 対数ゲイン → 線形ゲイン
    numScratchRows
};

/** dB → 対数振幅。-db/20 * ln(10)。 */
constexpr float kDbToLogAmplitude = -0.11512925f;

/** 強さ切替時に logGmin をランプさせる幅。弱(6dB)↔強(18dB) の全振れを
    ちょうど 40 ms（= mixStep と同じ時間）で渡り切る速さにする。 */
constexpr float kLogGminSpan = 1.3815511f;   // ln(10) * (18 - 6) / 20

/** 帯域別の雑音過剰推定 betaOS。

    denoise.json は「バンド境界を 4 ビンかけて線形補間」と書いているが、あれは
    46.875 Hz ビン（N=1024）前提で、Hz に直すと 187.5 Hz 幅の遷移という意味。
    我々のビン幅がちょうど 187.5 Hz なので、ビン数ではなく Hz で幅を持たせ直す。
    こうしておけば N=512 に切り替えてもカーブの形が変わらない。 */
constexpr float halfWidth = 93.75f;   // 遷移の半幅（全幅 187.5 Hz）

float betaEdge (float f, float centre, float lo, float hi) noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, (f - (centre - halfWidth)) / (2.0f * halfWidth));
    return lo + t * (hi - lo);
}

float betaOverEstimate (float hz) noexcept
{
    if (hz < 1000.0f - halfWidth) return 1.0f;
    if (hz < 1000.0f + halfWidth) return betaEdge (hz, 1000.0f, 1.0f, 1.2f);
    if (hz < 4000.0f - halfWidth) return 1.2f;
    if (hz < 4000.0f + halfWidth) return betaEdge (hz, 4000.0f, 1.2f, 1.5f);
    if (hz < 8000.0f - halfWidth) return 1.5f;
    if (hz < 8000.0f + halfWidth) return betaEdge (hz, 8000.0f, 1.5f, 2.0f);

    return 2.0f;
}

/** 再帰状態の初期化。reset() と NaN 検出時の復帰の両方から使う。 */
void initBinState (juce::AudioBuffer<float>& state, int numBins, double sampleRate) noexcept
{
    if (numBins <= 1)
        return;

    auto* s     = state.getWritePointer (rowS);
    auto* sPrev = state.getWritePointer (rowSprev);
    auto* sMin  = state.getWritePointer (rowSmin);
    auto* spp   = state.getWritePointer (rowSpp);
    auto* noise = state.getWritePointer (rowNoise);
    auto* gPrev = state.getWritePointer (rowGprev);
    auto* dd    = state.getWritePointer (rowDd);
    auto* beta  = state.getWritePointer (rowBeta);
    auto* delta = state.getWritePointer (rowDelta);

    const double binHz = sampleRate / (double) (2 * (numBins - 1));

    for (int k = 0; k < numBins; ++k)
    {
        s[k]     = 0.0f;
        sPrev[k] = 0.0f;
        sMin[k]  = 1.0e-12f;
        spp[k]   = 0.0f;
        noise[k] = 1.0e-12f;
        gPrev[k] = 1.0f;
        dd[k]    = 1.0f;

        const float hz = (float) (binHz * (double) k);

        beta[k]  = betaOverEstimate (hz);

        // 3 kHz 未満はしきい値を低くして「音声だ」と言いやすくする（歌の持続音を守る）。
        delta[k] = hz < 3000.0f ? 2.0f : 5.0f;
    }
}

} // namespace

//==============================================================================
NoiseSuppressor::Coefficients NoiseSuppressor::deriveCoefficients (double sampleRate, int hopSize) noexcept
{
    Coefficients c;

    const double sr = sampleRate > 0.0 ? sampleRate : kPreferredSampleRate;
    const int    hop = juce::jmax (1, hopSize);

    // ここが「写経禁止」の核心。dt は HOP=64 @48k で 1.3333 ms であって、
    // denoise.json が前提にしている 5.3333 ms ではない。
    c.dtMs = 1000.0 * (double) hop / sr;

    c.aS     = onePoleKeep (c.dtMs, 24.0);
    c.aP     = onePoleKeep (c.dtMs, 35.0);
    c.aD     = onePoleKeep (c.dtMs, 530.0);
    c.aDfast = onePoleKeep (c.dtMs, 60.0);

    // tau = 131 ms 相当。0.96 を写すと tau が 33 ms に落ちてミュージカルノイズが出る。
    c.aDD    = onePoleKeep (c.dtMs, 131.0);

    // Doblinger の参照値は 16 ms ホップでの 0.998 / 0.96。x' = x^(dt/16ms) で移す。
    c.gDob = (float) std::pow (0.998, c.dtMs / 16.0);
    c.bDob = (float) std::pow (0.96,  c.dtMs / 16.0);

    c.attackCoef  = 1.0f - onePoleKeep (c.dtMs, 3.0);
    c.releaseCoef = 1.0f - onePoleKeep (c.dtMs, 25.0);

    c.gateAttackCoef  = 1.0f - onePoleKeep (c.dtMs, 5.0);
    c.gateReleaseCoef = 1.0f - onePoleKeep (c.dtMs, 120.0);

    c.fastFrames = (int) std::lround (500.0 / c.dtMs);   // 0.5 s
    c.holdFrames = (int) std::lround (250.0 / c.dtMs);   // 250 ms
    c.mixStep    = (float) (c.dtMs / 40.0);              // 40 ms 直線ランプ

   #if JUCE_DEBUG
    if (std::abs (sr - 48000.0) < 1.0e-6 && hop == 64)
    {
        // ヘッダの参考値と一度だけ突き合わせる（再導出が壊れたら即わかるように）。
        auto close = [] (float a, float b) { return std::abs (a - b) <= 5.0e-4f; };

        jassert (close (c.aS,     0.9460f));
        jassert (close (c.aP,     0.9626f));
        jassert (close (c.aD,     0.99749f));
        jassert (close (c.aDfast, 0.97803f));
        jassert (close (c.aDD,    0.98987f));
        jassert (close (c.gDob,   0.999833f));
        jassert (close (c.bDob,   0.996604f));
        jassert (close (c.attackCoef,      0.3588f));
        jassert (close (c.releaseCoef,     0.05194f));
        jassert (close (c.gateAttackCoef,  0.23407f));
        jassert (close (c.gateReleaseCoef, 0.01105f));
        jassert (close (c.mixStep,         0.033333f));
        jassert (c.fastFrames == 375);
        jassert (c.holdFrames == 188);
        juce::ignoreUnused (close);
    }
   #endif

    return c;
}

//==============================================================================
float NoiseSuppressor::expIntegralE1 (float x) noexcept
{
    if (x < 1.0e-6f) x = 1.0e-6f;
    if (x > 30.0f)   return 0.0f;   // exp(-30)/30 はどのみちアンダーフローする

    if (x <= 1.0f)
        return -std::log (x) - 0.57721566f
             + x * ( 0.99999193f + x * (-0.24991055f
             + x * ( 0.05519968f + x * (-0.00976004f
             + x *   0.00107857f))));

    return std::exp (-x) / x
         * ((x * (x + 2.334733f) + 0.250621f)
         /  (x * (x + 3.330657f) + 1.681534f));
}

//==============================================================================
NoiseSuppressor::NoiseSuppressor() = default;
NoiseSuppressor::~NoiseSuppressor() = default;

//==============================================================================
void NoiseSuppressor::prepare (double newSampleRate, int fftSize, int hopSize, int maxBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : kPreferredSampleRate;

    wola.prepare (fftSize, hopSize, maxBlockSize);
    numBins = wola.getNumBins();

    coef = deriveCoefficients (sampleRate, hopSize);

    state.setSize (numStateRows, numBins);
    scratch.setSize (numScratchRows, numBins);

    const double binHz = sampleRate / (double) fftSize;

    gateBinLo = juce::jlimit (1, juce::jmax (1, numBins - 2),
                              (int) std::lround (kGateBandLoHz / binHz));
    gateBinHi = juce::jlimit (gateBinLo, juce::jmax (gateBinLo, numBins - 2),
                              (int) std::lround (kGateBandHiHz / binHz));

    reset();
}

void NoiseSuppressor::reset() noexcept
{
    wola.reset();

    initBinState (state, numBins, sampleRate);

    gateGain      = 1.0f;
    vadOpen       = false;
    holdCounter   = 0;
    warmupCounter = coef.fastFrames;
    allZeroFrames = 0;

    const float db = maxAttenuationDb (static_cast<NoiseStrength> (strengthTarget.load (std::memory_order_relaxed)));
    logGMin = db * kDbToLogAmplitude;
    gMin    = std::exp (logGMin);

    // 新しいストリームではランプしない（起動直後に 40 ms かけて開くと初手が抜ける）。
    mix = enabledTarget.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
}

//==============================================================================
void NoiseSuppressor::setEnabled (bool shouldBeEnabled) noexcept
{
    enabledTarget.store (shouldBeEnabled, std::memory_order_relaxed);
}

void NoiseSuppressor::setStrength (NoiseStrength s) noexcept
{
    strengthTarget.store (static_cast<int> (s), std::memory_order_relaxed);
}

//==============================================================================
void NoiseSuppressor::process (const float* input, float* output, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    if (numSamples <= 0)
        return;

    if (! wola.isPrepared() || numBins <= 1)
    {
        if (output != input)
            juce::FloatVectorOperations::copy (output, input, numSamples);

        return;
    }

    // OFF でも WOLA と推定器は回し続ける。切り離すと遅延が変わり、
    // 再有効化のたびに 0.5 秒の学習し直しとクリックが出る。
    wola.process (input, output, numSamples, *this);
}

//==============================================================================
void NoiseSuppressor::processSpectrum (float* bins, int n) noexcept
{
    jassert (n == numBins);

    if (n <= 1)
        return;

    auto* smoothPow = state.getWritePointer (rowS);
    auto* prevPow   = state.getWritePointer (rowSprev);
    auto* minPow    = state.getWritePointer (rowSmin);
    auto* spp       = state.getWritePointer (rowSpp);
    auto* noisePow  = state.getWritePointer (rowNoise);
    auto* prevGain  = state.getWritePointer (rowGprev);
    auto* ddMemory  = state.getWritePointer (rowDd);
    const auto* beta  = state.getReadPointer (rowBeta);
    const auto* delta = state.getReadPointer (rowDelta);

    auto* power    = scratch.getWritePointer (rowPow);
    auto* smoothed = scratch.getWritePointer (rowSf);
    auto* noiseOs  = scratch.getWritePointer (rowNb);
    auto* gain     = scratch.getWritePointer (rowGain);

    //--------------------------------------------------------------------------
    // 0. パワースペクトル
    float frameEnergy = 0.0f;

    for (int k = 0; k < n; ++k)
    {
        const float re = bins[2 * k];
        const float im = bins[2 * k + 1];
        power[k] = re * re + im * im;
        frameEnergy += power[k];
    }

    // NaN/Inf が再帰状態に入ると二度と抜けられず、アプリ再起動まで無音になる。
    // フレームを捨てて推定器だけ作り直す（WOLA の位相状態には触らない）。
    if (! std::isfinite (frameEnergy))
    {
        for (int k = 0; k < 2 * n; ++k)
            bins[k] = 0.0f;

        initBinState (state, n, sampleRate);
        gateGain = 1.0f;
        vadOpen = false;
        holdCounter = 0;
        warmupCounter = coef.fastFrames;
        return;
    }

    // 周波数平滑。N=256 ではビン幅 187.5 Hz なので denoise.json の [0.25,0.5,0.25]
    // （= 375 Hz 幅）は男声の倍音を潰す。ゲインと同じ弱めたカーネルを使う。
    if (n >= 3)
    {
        smoothed[0]     = power[0];
        smoothed[n - 1] = power[n - 1];

        for (int k = 1; k < n - 1; ++k)
            smoothed[k] = kFreqSmoothSide * power[k - 1]
                        + kFreqSmoothCentre * power[k]
                        + kFreqSmoothSide * power[k + 1];
    }
    else
    {
        for (int k = 0; k < n; ++k)
            smoothed[k] = power[k];
    }

    //--------------------------------------------------------------------------
    // 全ゼロ入力ガード。ここを抜くとドライバのグリッチで「雑音 = 0」を学習し、
    // 次の実音がフルゲインで飛び出す。
    if (frameEnergy < kAllZeroFrameEnergy)
        ++allZeroFrames;
    else
        allZeroFrames = 0;

    const bool freezeEstimator = allZeroFrames >= kAllZeroFramesToFreeze;

    const bool  fast = warmupCounter > 0;
    const float aDn  = fast ? coef.aDfast : coef.aD;
    const float dobK = (1.0f - coef.gDob) / juce::jmax (1.0e-9f, 1.0f - coef.bDob);

    // 強さの変更は log 領域でランプする（Gmin を跳ばすと段差が聞こえる）。
    const float targetLogGmin =
        maxAttenuationDb (static_cast<NoiseStrength> (strengthTarget.load (std::memory_order_relaxed)))
        * kDbToLogAmplitude;

    const float gMinStep = kLogGminSpan * coef.mixStep;
    logGMin += juce::jlimit (-gMinStep, gMinStep, targetLogGmin - logGMin);
    gMin = std::exp (logGMin);

    float bandPower = 0.0f, bandNoise = 0.0f;

    //--------------------------------------------------------------------------
    for (int k = 0; k < n; ++k)
    {
        // 1. 平滑パワー
        smoothPow[k] = coef.aS * smoothPow[k] + (1.0f - coef.aS) * smoothed[k];

        // 2. Doblinger 連続最小値追跡
        if (minPow[k] < smoothPow[k])
            minPow[k] = coef.gDob * minPow[k] + dobK * (smoothPow[k] - coef.bDob * prevPow[k]);
        else
            minPow[k] = smoothPow[k];

        // 追従項が負に振れると比が負になり「音声なし」と誤判定して声を雑音として学習する。
        minPow[k] = juce::jmax (minPow[k], kEps);

        prevPow[k] = smoothPow[k];

        // 3. 音声存在確率（MCRA）
        const float ratio     = smoothPow[k] / juce::jmax (minPow[k], kEps);
        const float indicator = ratio > delta[k] ? 1.0f : 0.0f;
        spp[k] = coef.aP * spp[k] + (1.0f - coef.aP) * indicator;

        // 4. 雑音パワー更新。p → 1 で ad → 1 になり、音声中は自動的に凍結される。
        if (! freezeEstimator)
        {
            const float ad = aDn + (1.0f - aDn) * spp[k];
            noisePow[k] = ad * noisePow[k] + (1.0f - ad) * power[k];
        }

        noiseOs[k] = beta[k] * noisePow[k] + kEps;

        // 5. 事後 SNR と決定指向の事前 SNR
        const float gamma = juce::jmin (power[k] / noiseOs[k], kGammaMax);
        float xi = coef.aDD * ddMemory[k] + (1.0f - coef.aDD) * juce::jmax (gamma - 1.0f, 0.0f);
        xi = juce::jlimit (kXiMin, kXiMax, xi);

        // 6. OM-LSA。log 領域のまま次段の周波数平滑へ渡す。
        const float r = xi / (1.0f + xi);
        const float v = r * gamma;

        gain[k] = spp[k] * (std::log (r) + 0.5f * expIntegralE1 (v))
                + (1.0f - spp[k]) * logGMin;

        if (k >= gateBinLo && k <= gateBinHi)
        {
            bandPower += power[k];
            bandNoise += noiseOs[k];
        }
    }

    //--------------------------------------------------------------------------
    // 7. 対数ゲインの周波数平滑。単独ビンのゲインスパイク = ミュージカルノイズなので、
    //    3 タップで「1 ビンだけ突出する」ことを構造的に不可能にする。
    if (n >= 3)
    {
        float prev = gain[0];

        for (int k = 1; k < n - 1; ++k)
        {
            const float cur = gain[k];
            gain[k] = kFreqSmoothSide * prev + kFreqSmoothCentre * cur + kFreqSmoothSide * gain[k + 1];
            prev = cur;
        }
    }

    //--------------------------------------------------------------------------
    // 8. フロア + 非対称時間平滑（開くのは速く、閉じるのは遅く）
    for (int k = 0; k < n; ++k)
    {
        float g = std::exp (gain[k]);
        g = juce::jlimit (gMin, 1.0f, g);

        const float a = g > prevGain[k] ? coef.attackCoef : coef.releaseCoef;
        g = prevGain[k] + a * (g - prevGain[k]);

        // DD 再帰にはゲート前・ミックス前の「生の」ゲインを入れる。
        // 混ぜたゲインを戻すと OFF 中に事前 SNR が汚染され、ON に戻して 300 ms 破綻する。
        ddMemory[k] = juce::jmin (kXiMax, (g * g * power[k]) / noiseOs[k]);
        prevGain[k] = g;
        gain[k] = g;
    }

    gain[0]     = gMin;   // DC
    gain[n - 1] = gMin;   // Nyquist
    prevGain[0]     = gMin;
    prevGain[n - 1] = gMin;

    //--------------------------------------------------------------------------
    // 9. フレームゲート（ヒステリシス + ホールド）。無音区間の残留ヒスを消す。
    const float snrDb = 10.0f * std::log10 ((bandPower + 1.0e-12f) / (bandNoise + 1.0e-12f));

    if (snrDb > kGateOpenDb)
    {
        vadOpen = true;
        holdCounter = coef.holdFrames;
    }
    else if (snrDb < kGateCloseDb)
    {
        if (--holdCounter <= 0)
        {
            vadOpen = false;
            holdCounter = 0;
        }
    }

    // 収束前はゲートを開けたままにする。でないと最初の一言が飲み込まれる。
    const float gateTarget = (vadOpen || fast) ? 1.0f : kGateFloor;
    gateGain += (gateTarget > gateGain ? coef.gateAttackCoef : coef.gateReleaseCoef)
              * (gateTarget - gateGain);

    //--------------------------------------------------------------------------
    // 10. ON/OFF の 40 ms ランプ。mix == 0 で Geff が厳密に 1.0 になりビット透過。
    const float mixTarget = enabledTarget.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
    mix = juce::jlimit (0.0f, 1.0f,
                        mix + juce::jlimit (-coef.mixStep, coef.mixStep, mixTarget - mix));

    if (mix > 0.0f)
    {
        for (int k = 0; k < n; ++k)
        {
            const float effective = 1.0f + mix * (gain[k] * gateGain - 1.0f);
            bins[2 * k]     *= effective;
            bins[2 * k + 1] *= effective;
        }
    }

    if (warmupCounter > 0)
        --warmupCounter;
}

} // namespace kvc
