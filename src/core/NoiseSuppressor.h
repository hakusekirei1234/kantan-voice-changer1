// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "core/Parameters.h"
#include "core/Wola.h"

#include <atomic>
#include <cmath>

namespace kvc
{

//==============================================================================
/** OM-LSA + MCRA(Doblinger 連続最小値追跡) スペクトル抑圧。定常ノイズ専用。

    既定は N=256 / HOP=64（遅延 5.33 ms）。詳細設定で N=512 / HOP=128 も選べるが、
    切り替えるとアルゴリズム遅延が変わるので、必ずエンジンを作り直すこと。

    ★時定数の再導出について（絶対に守ること）
      denoise.json の数値は N=1024 / HOP=256（dt = 5.333 ms）を前提にしている。
      我々の既定は HOP=64 で dt = 1.3333 ms なので、係数を写経すると時定数が
      1/4 になる。特に aDD を 0.96 のままにすると tau が 131 ms → 33 ms に落ち、
      ミュージカルノイズが出る。必ず deriveCoefficients() で dt から作ること。

    ミュージカルノイズ対策は積み重ねで効く。順に効果が大きい:
      1. 決定指向(DD)の事前 SNR   ... これだけは省略不可
      2. 非ゼロのスペクトルフロア Gmin ... 0 にしてはいけない
      3. SPP 重み付け G = G_LSA^p * Gmin^(1-p)
      4. 対数ゲインの周波数平滑
      5. 非対称の時間平滑（開くの速く、閉じるの遅く）
      6. LSA（振幅ではなく対数振幅の誤差最小化）
      7. 75% オーバーラップ + 合成窓（Wola が担保）
      8. 帯域依存の控えめな過剰推定 betaOS

    ★周波数平滑タップの注意
      N=256 ではビン幅が 187.5 Hz あるので、denoise.json の [0.25,0.50,0.25] は
      375 Hz 幅になり男声の倍音を潰す。既定は [0.15,0.70,0.15] に弱める
      （実聴で決める余地を残す。kFreqSmoothSide を触ること）。

    ★推定器は OFF 中も回し続ける
      バイパスすると STFT 状態と雑音推定がずれ、再有効化時にクリックと
      0.5 秒の「学習し直し」が出る。mix=0 のとき Geff は厳密に 1.0 になり
      ビット透過になる。DD 再帰にはミックス前・ゲート前の生ゲインを入れること。

    スレッド契約:
      prepare / reset ... メッセージスレッド専用（確保する）
      setEnabled / setStrength ... どのスレッドからでも（atomic）
      process         ... オーディオスレッド専用
*/
class NoiseSuppressor : private Wola::FrameProcessor
{
public:
    //==========================================================================
    /** dt から導出する係数一式。数値を直書きしないこと。 */
    struct Coefficients
    {
        double dtMs = 0.0;

        float aS = 0.0f;        ///< A(24 ms)  電力平滑
        float aP = 0.0f;        ///< A(35 ms)  音声存在確率
        float aD = 0.0f;        ///< A(530 ms) 雑音更新
        float aDfast = 0.0f;    ///< A(60 ms)  起動直後の高速追従
        float aDD = 0.0f;       ///< A(131 ms) 決定指向の事前 SNR

        float gDob = 0.0f;      ///< 0.998 ^ (dt/16ms)
        float bDob = 0.0f;      ///< 0.96  ^ (dt/16ms)

        float attackCoef = 0.0f;   ///< 1 - A(3 ms)   ゲインが上がる方向
        float releaseCoef = 0.0f;  ///< 1 - A(25 ms)  ゲインが下がる方向

        float gateAttackCoef = 0.0f;   ///< 1 - A(5 ms)
        float gateReleaseCoef = 0.0f;  ///< 1 - A(120 ms)

        int   fastFrames = 0;      ///< 0.5 s / dt
        int   holdFrames = 0;      ///< 250 ms / dt
        float mixStep = 0.0f;      ///< dt / 40 ms
    };

    /** A(t) = exp(-dt / t)。t はミリ秒。 */
    static float onePoleKeep (double dtMs, double tauMs) noexcept
    {
        return static_cast<float> (std::exp (-dtMs / tauMs));
    }

    /** HOP と fs から全係数を作る。
        参考値（fs=48000, HOP=64, dt=1.3333 ms）:
          aS 0.9460 / aP 0.9626 / aD 0.99749 / aDfast 0.97803 / aDD 0.98987
          gDob 0.999833 / bDob 0.996604
          attack 0.3588 / release 0.05194
          gateAttack 0.23407 / gateRelease 0.01105
          fastFrames 375 / holdFrames 188 / mixStep 0.033333
        実装後にこの表と一致することを一度だけ assert すること。 */
    static Coefficients deriveCoefficients (double sampleRate, int hopSize) noexcept;

    //==========================================================================
    // 固定パラメータ

    static constexpr float kXiMin = 0.0031623f;   // -25 dB
    static constexpr float kXiMax = 10000.0f;     // +40 dB
    static constexpr float kGammaMax = 1000.0f;   // +30 dB
    static constexpr float kEps = 1.0e-20f;

    /** 周波数平滑タップ（対数ゲインに対して）。N=256 のビン幅 187.5 Hz を考慮して弱めてある。 */
    static constexpr float kFreqSmoothSide = 0.15f;
    static constexpr float kFreqSmoothCentre = 0.70f;

    static constexpr float kGateOpenDb = 6.0f;
    static constexpr float kGateCloseDb = 3.0f;
    static constexpr float kGateFloor = 0.10f;      // 追加 -20 dB
    static constexpr float kGateBandLoHz = 200.0f;
    static constexpr float kGateBandHiHz = 6000.0f;

    static constexpr float kAllZeroFrameEnergy = 1.0e-14f;
    static constexpr int   kAllZeroFramesToFreeze = 5;

    /** 弱/標準/強 の最大減衰量 (dB)。Gmin = 10^(-dB/20)、決してゼロにしない。 */
    static float maxAttenuationDb (NoiseStrength s) noexcept
    {
        switch (s)
        {
            case NoiseStrength::weak:   return 6.0f;
            case NoiseStrength::strong: return 18.0f;
            case NoiseStrength::normal:
            default:                    return 12.0f;
        }
    }

    /** 指数積分 E1(x)。Abramowitz & Stegun 5.1.53 / 5.1.56。ライブラリ不要。 */
    static float expIntegralE1 (float x) noexcept;

    //==========================================================================
    NoiseSuppressor();
    ~NoiseSuppressor() override;

    /** @param fftSize kNoiseFftLowLatency(256) か kNoiseFftHighQuality(512)。
        @param hopSize fftSize / 4。 */
    void prepare (double sampleRate, int fftSize, int hopSize, int maxBlockSize);

    /** サンプルレート変更・デバイス変更・ストリーム再開のたびに必ず呼ぶ。
        高速追従ウィンドウに入り直す。 */
    void reset() noexcept;

    void setEnabled (bool shouldBeEnabled) noexcept;
    void setStrength (NoiseStrength) noexcept;

    bool isEnabled() const noexcept { return enabledTarget.load (std::memory_order_relaxed); }

    /** input と output は同一バッファでも構わない。 */
    void process (const float* input, float* output, int numSamples) noexcept;

    /** = fftSize。 */
    int getLatencySamples() const noexcept { return wola.getLatencySamples(); }

    const Coefficients& getCoefficients() const noexcept { return coef; }

private:
    void processSpectrum (float* complexBins, int numBins) noexcept override;

    Wola wola;
    Coefficients coef;

    double sampleRate = 48000.0;
    int    numBins = 0;
    int    gateBinLo = 0, gateBinHi = 0;

    // 全て numBins 長。prepare で一度だけ確保する。
    juce::AudioBuffer<float> state;   // 行: S, Sprev, Smin, p, N, Gprev, ddPrev, betaOS, delta
    juce::AudioBuffer<float> scratch; // 行: P, Sf, Nb, logG

    float gateGain = 1.0f;
    bool  vadOpen = false;
    int   holdCounter = 0;
    int   warmupCounter = 0;
    int   allZeroFrames = 0;

    float mix = 0.0f;
    float gMin = 0.2512f;
    float logGMin = -1.3818f;

    std::atomic<bool> enabledTarget { false };
    std::atomic<int>  strengthTarget { static_cast<int> (NoiseStrength::normal) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoiseSuppressor)
};

} // namespace kvc
