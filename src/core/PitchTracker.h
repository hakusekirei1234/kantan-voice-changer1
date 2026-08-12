// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>

namespace kvc
{

//==============================================================================
/** 48 kHz → 8 kHz の 6 倍デシメーション + CMNDF(YIN) ピッチ検出。

    追加遅延はゼロ。既にバッファ済みの過去データしか読まないため。
    T0 の推定が 10〜15 ms 遅れるのは、話声の F0 が最大でも毎秒 1 オクターブ程度しか
    動かないので知覚閾値のはるか下。

    パラメータ（dsp.json 準拠）:
      デシメーション    6 倍（31 タップハーフバンドのカスケード、遮断 3.6 kHz）
      解析レート        8000 Hz
      ラグ範囲          16..133 @8k = 500..60 Hz
      解析窓            266 サンプル @8k（最大 2 周期）
      ホップ            128 サンプル @48k = 2.667 ms
      有声判定          d < 0.15 → v=1、d > 0.45 → v=0、その間は線形（連続値）
      後処理            ラグの 5 フレームメディアン + オクターブガード
                        （global min の 1.2 倍以内の候補のうち前回ラグに最も近いものを選ぶ）
      v < 0.4 のときは T0 を追わずに前回値を保持する

    ★オクターブエラーは PSOLA の音質を一発で壊す唯一の要因なので、
      メディアン・ガード・クランプ・ホールドの 4 つは省略不可。

    スレッド契約:
      prepare / reset ... メッセージスレッド専用（確保する）
      pushBlock / getCurrent / 8k リングのアクセサ ... オーディオスレッド専用
*/
class PitchTracker
{
public:
    static constexpr int kDecimationFactor = 6;
    static constexpr double kAnalysisRate = 8000.0;

    static constexpr int kMinLag8 = 16;    // 500 Hz
    static constexpr int kMaxLag8 = 133;   // 60 Hz
    static constexpr int kWindow8 = 266;   // 2 * kMaxLag8
    static constexpr int kHop48 = 128;

    static constexpr int kRing8Size = 2048;   // 2 のべき乗。約 256 ms @8k。
    static constexpr int kRing8Mask = kRing8Size - 1;

    static constexpr int kMedianLength = 5;

    static constexpr float kVoicedThreshold = 0.15f;
    static constexpr float kUnvoicedThreshold = 0.45f;
    static constexpr float kOctaveGuardRatio = 1.2f;
    static constexpr float kHoldVoicingBelow = 0.4f;

    static constexpr float kMinPeriod48 = 96.0f;    // 500 Hz
    static constexpr float kMaxPeriod48 = 800.0f;   // 60 Hz

    struct Result
    {
        float periodSamples48 = 240.0f;  ///< T0（48 kHz サンプル単位）。常にクランプ済み。
        float voicing = 0.0f;            ///< v、0..1 の連続値。ハード切替はしない。
    };

    PitchTracker();
    ~PitchTracker();

    /** sampleRate が 48 kHz でない場合はデシメーション比を再計算すること。 */
    void prepare (double sampleRate);
    void reset() noexcept;

    /** 入力ブロックをリングに積み、必要なぶんだけ解析フレームを回す。
        確保もロックもしない。 */
    void pushBlock (const float* mono, int numSamples) noexcept;

    Result getCurrent() const noexcept { return current; }

    //==========================================================================
    // PitchFormantShifter が相関ロックに使う 8 kHz リングへのアクセス。
    // 別クラスに丸ごとコピーさせないための、意図的な公開。

    const float* getDecimatedRing() const noexcept { return ring8; }
    static constexpr int getDecimatedRingSize() noexcept { return kRing8Size; }
    static constexpr int getDecimatedRingMask() noexcept { return kRing8Mask; }

    /** 8 kHz リングに書き込まれた総サンプル数（絶対位置）。 */
    int64_t getDecimatedWritePos() const noexcept { return write8; }

    /** 実デシメーション比。48 kHz なら 6、96 kHz なら 12。
        `getDecimatedWritePos() == getInputWritePos() / getDecimationFactor()` が常に成立する。
        相関ロックで入力位置↔8k リング位置を変換するときは 6 を直書きせずこれを使うこと。 */
    int getDecimationFactor() const noexcept { return decimationFactor; }

    /** 実解析レート（= sourceRate / getDecimationFactor()）。48 kHz なら厳密に 8000。 */
    double getAnalysisRate() const noexcept { return analysisRate; }

    /** 48 kHz 入力に書き込まれた総サンプル数（絶対位置）。 */
    int64_t getInputWritePos() const noexcept { return write48; }

private:
    /** デシメーション 1 段あたりの FIR タップ数上限。88.2 kHz のように比が素数で
        2 段に割れないときだけこの上限に当たり、そのぶん折り返しが増える。 */
    static constexpr int kMaxFirTaps = 255;

    void analyseFrame() noexcept;

    double sourceRate = 48000.0;

    float ring8[kRing8Size] {};
    int64_t write8 = 0;
    int64_t write48 = 0;
    int64_t nextAnalysisAt48 = 0;

    // 4 行: 0,1 = 各段の FIR 係数 / 2,3 = 各段の遅延線（長さ 2N の二重化リング）。
    // reset() で消してよいのは遅延線の 2 行だけ。
    juce::AudioBuffer<float> decimationState;
    juce::AudioBuffer<float> scratch;           // CMNDF の作業領域

    float  lagHistory[kMedianLength] {};
    int    lagHistoryPos = 0;
    float  previousLag8 = 40.0f;

    // prepare() で実レートから導出する。48000 決め打ちにしないため。
    int    decimationFactor = kDecimationFactor;
    double analysisRate = kAnalysisRate;
    int    hopSource = kHop48;
    int    minLag = kMinLag8;
    int    maxLag = kMaxLag8;
    int    windowLength = kWindow8;
    float  minPeriodSource = kMinPeriod48;
    float  maxPeriodSource = kMaxPeriod48;

    int    numStages = 2;
    int    stageFactor[2] { 3, 2 };
    int    stageTaps[2] { 0, 0 };
    int    stageWrite[2] { 0, 0 };
    int    stagePhase[2] { 0, 0 };

    int    lagHistoryCount = 0;
    int    unvoicedRun = 0;
    bool   previousLagValid = false;

    Result current;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTracker)
};

} // namespace kvc
