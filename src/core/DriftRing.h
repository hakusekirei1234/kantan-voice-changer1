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
/** 単一生産者・単一消費者のリングバッファ + Lagrange リサンプラ + P 制御ドリフト補正。

    3 つの独立クロックがある以上、スレーブレグ 1 本につき必ず 1 個必要。
    これは後から足せない類の部品なので、パススルー段階から配線しておくこと。

    制御則（DECISIONS.md 確定版。PI ではなく P のみ。積分器ワインドアップの
    バグを持ち込まないため。定常オフセットが数十フレーム残るのは無害）:

        fill  = getNumReady();
        alpha = slaveBlock / (slaveRate * kTauSeconds);
        avgFill += alpha * (fill - avgFill);
        e     = avgFill - targetFill;                                 // frames
        corr  = clamp(e / (slaveRate * kSettleSeconds), -0.004, +0.004);
        speedRatio = nominalRatio * (1.0 + corr);
        used  = interpolator.process(speedRatio, staging, out, numOut);
        finishedRead(used);

    符号の確認: バッファが溜まり過ぎ (e>0) → corr>0 → speedRatio>1 →
    出力 1 フレームあたり入力を多く消費 → 溜まりが減る。正しい。

    アンダーラン時は不足分を無音で埋めた上で「ハードリシンク」
    （fifo.reset → targetFill まで無音プリフィル → interpolator.reset）。
    段階的な追いつきはしない（動く目標を追いかけて何度もグリッチする）。
    オーバーラン時は最古のフレームを捨てる（遅延を有界に保つ）。

    スレッド契約:
      prepare / prefillSilence / reset / setFillMode ... メッセージスレッド専用、
                                                         かつ両デバイス停止中のみ
      write  ... マスター（入力デバイス）のオーディオスレッド専用
      read   ... このリングを持つスレーブ 1 本のオーディオスレッド専用
      getDiagnostics / getTargetFill ... どのスレッドからでも（atomic 読み）
*/
class DriftRing
{
public:
    /** 容量。8192 フレーム = 48 kHz で 170 ms。2 のべき乗であること。 */
    static constexpr int kCapacity = 8192;
    static constexpr int kMaxChannels = 2;

    /** リサンプラの 5 点カーネルぶんの履歴余裕。 */
    static constexpr int kResamplerGuardSamples = 8;

    static constexpr double kTauSeconds = 3.0;      ///< fill 平滑の時定数
    static constexpr double kSettleSeconds = 10.0;  ///< 誤差を解消する目標時間
    static constexpr double kMaxCorrection = 0.004; ///< ±4000 ppm ハードクランプ

    /** 適応セーフティネット: 直近 30 秒でアンダーランが 3 回を超えたら
        targetFill を 1.25 倍する（自動で下げることはしない）。 */
    static constexpr double kAdaptWindowSeconds = 30.0;
    static constexpr int    kAdaptUnderrunThreshold = 3;
    static constexpr double kAdaptGrowthFactor = 1.25;

    enum class FillMode
    {
        stable = 0,     ///< 安定優先: targetFill = masterBlock + 2*slaveBlock + 64
        lowLatency = 1  ///< 低遅延優先: targetFill = masterBlock + slaveBlock + 32
    };

    struct Diagnostics
    {
        float   ppm = 0.0f;         ///< (speedRatio/nominalRatio - 1) * 1e6
        int     fill = 0;           ///< 直近の getNumReady()
        float   avgFill = 0.0f;
        int     targetFill = 0;
        int32_t underruns = 0;
        int32_t overruns = 0;
        bool    pinnedAtClamp = false;  ///< 30 秒以上クランプに張り付き = レート不一致の疑い
    };

    DriftRing();
    ~DriftRing();

    //==========================================================================
    /** 確保はここでだけ行う。両デバイスが停止している間にメッセージスレッドから。

        @param numChannels   リングが保持するチャンネル数（本アプリでは 1）
        @param masterRate    入力デバイスが実際に open できたレート
        @param slaveRate     このスレーブが実際に open できたレート
        @param masterBlock   入力デバイスの実測ブロック長
        @param slaveBlock    スレーブの実測ブロック長
        @param mode          targetFill の式の選択

        nominalRatio は masterRate / slaveRate。48 kHz で揃っていれば 1.0 になり、
        微小なドリフト補正だけが効く。requested ではなく必ず
        getCurrentSampleRate() / getCurrentBufferSizeSamples() の実測値を渡すこと。
    */
    void prepare (int numChannels,
                  double masterRate, double slaveRate,
                  int masterBlock, int slaveBlock,
                  FillMode mode);

    /** targetFill フレームぶんの無音を書き込む。スレーブを start() する直前に。 */
    void prefillSilence();

    /** 全状態を初期化して無音プリフィルまで戻す。デバイス停止中のみ。 */
    void reset();

    bool isPrepared() const noexcept { return prepared; }

    //==========================================================================
    // --- マスターのオーディオスレッド ---

    /** numSourceChannels がリングのチャンネル数より少なければ ch0 を複製する。
        空きが足りなければ最古を捨てて書く（オーバーランカウンタを進める）。 */
    void write (const float* const* source, int numSourceChannels, int numSamples) noexcept;

    //==========================================================================
    // --- スレーブのオーディオスレッド ---

    /** dest を numSamples ぶん埋める。ここでドリフトリサンプルが走る。
        リングがモノラルで dest が複数チャンネルなら ch0 を全チャンネルに複製する。
        不足時は無音を埋めてハードリシンクする。 */
    void read (float* const* dest, int numDestChannels, int numSamples) noexcept;

    //==========================================================================
    Diagnostics getDiagnostics() const noexcept;
    int  getTargetFill() const noexcept { return targetFill.load (std::memory_order_relaxed); }
    int  getNumChannels() const noexcept { return numChannels; }

    /** targetFill の式（DECISIONS.md 3 節）。[256, kCapacity/4] にクランプ。 */
    static int computeTargetFill (int masterBlock, int slaveBlock, FillMode mode) noexcept;

private:
    juce::AudioBuffer<float> storage;                      // kCapacity フレーム
    juce::AudioBuffer<float> staging;                      // FIFO の 2 ブロックを連結する場所
    juce::AbstractFifo fifo { kCapacity };
    juce::OwnedArray<juce::Interpolators::Lagrange> resamplers;   // チャンネルごとに 1 本

    int    numChannels = 0;
    double nominalRatio = 1.0;
    double slaveRate = 48000.0;
    int    slaveBlock = 0;
    int    masterBlock = 0;
    FillMode fillMode = FillMode::stable;
    bool   prepared = false;

    // スレーブスレッド専有
    float  avgFill = 0.0f;
    double smoothAlpha = 0.0;
    int    underrunsInWindow = 0;
    double windowElapsedSeconds = 0.0;
    double clampedSeconds = 0.0;

    /** 1 回のリサンプル呼び出しで作る出力の上限。staging の確保長はこれに
        最大 speedRatio を掛けて決まる。 */
    int    maxChunkOut = 0;

    /** ハードリシンク後の「無音プリフィル」残量。リングへ書けるのは生産者だけ
        （validEnd は生産者専有）なので、消費者側は無音を FIFO に書き戻す代わりに
        この残量ぶんだけ staging の先頭にゼロを並べて消化する。効果は同じで、
        SPSC 契約を壊さない。 */
    int    silenceDebt = 0;

    std::atomic<int>     targetFill { 256 };
    std::atomic<float>   ppmNow { 0.0f };
    std::atomic<float>   avgFillPublished { 0.0f };
    std::atomic<int>     fillPublished { 0 };
    std::atomic<int32_t> underruns { 0 };
    std::atomic<int32_t> overruns { 0 };
    std::atomic<bool>    pinned { false };

    void hardResync() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DriftRing)
};

} // namespace kvc
