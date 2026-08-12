// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "core/PitchTracker.h"

#include <cstdint>

namespace kvc
{

//==============================================================================
/** 相関ロック TD-PSOLA + グレイン内リサンプル（= フォルマント独立シフト）。

    フェーズボコーダは採らない。男声の倍音を分離するには N=2048（42.7 ms）が要り、
    N=1024 に落とすと 46.9 Hz ビンで倍音が潰れて位相ロックが機能せず、まさに
    ユーザーが拒否した「ガビガビ」が出るため（DECISIONS.md 矛盾B）。

    原理:
      ピッチ  = グレインの「間隔」   → 間隔を変える
      フォルマント = グレインの「中身の時間軸」 → 中身だけを F 倍で読む
    ストリーム全体はリサンプルしないので、2 つのつまみは本当に直交する。

    固定量:
      LOOKAHEAD = 512 サンプル。これがこのクラスの唯一のアルゴリズム遅延であり、
      ピッチ量・フォルマント量・バイパス状態のいずれでも変化しない。
      可変にすると送信/モニターの遅延補正が切替のたびにずれてプチッと鳴る。

    実装上、外してはいけない点:
      - グレイン内リサンプルは 4 点 3 次 Hermite（Catmull-Rom）。
      - F > 1.05 のときだけグレインごとにバイカッド LP（fc = 0.45*fs/F, Q=0.7071）。
        省くと +12 半音フォルマントで 12〜20 kHz の歯擦音が折り返してざらつく。決して任意にしない。
      - OLA 正規化は「リサンプル後の窓」を積算した winAccum で割る。
        解析的なゲイン補正で代用してはいけない（任意の h・間隔・F で破綻する）。
        除数は 0.10 でクランプする。
      - P_eff = 1 + v*(P-1)。無声区間にはピッチ間隔を押し付けない（/s/ が金属的になる）。
      - 無声時のマーク位置に uniform(-1,1) * (1-v) * T0/8 のジッタを足す。
      - トランジェント検出（直近 3 ms のエネルギーが直前 6 ms の 4 倍超）で
        相関探索とグレイン複製をやめ、h を 0.5*T0 にして nextEpoch を出力前線に再アンカーする。
      - ピッチ/フォルマントの半音値はグレインごとに 1 回だけ評価し、
        1 グレインあたり 1.0 半音のスルーレート制限をかける。

    スレッド契約:
      prepare / reset ... メッセージスレッド専用（確保する）
      set* / process  ... オーディオスレッド専用（確保・ロックなし）
      getLatencySamples ... どこからでも（常に定数）
*/
class PitchFormantShifter
{
public:
    static constexpr int kLookaheadSamples = 512;   // = kPsolaLookaheadSamples
    static constexpr int kRingSize = 8192;          // 2 のべき乗
    static constexpr int kRingMask = kRingSize - 1;

    static constexpr float kMinSemitones = -12.0f;
    static constexpr float kMaxSemitones = 12.0f;

    static constexpr float kParamTauSeconds = 0.020f;      ///< 半音値の一次遅れ
    static constexpr float kMaxSemitoneStepPerGrain = 1.0f;
    static constexpr float kWinAccumFloor = 0.10f;
    static constexpr float kAntiAliasThreshold = 1.05f;    ///< F がこれを超えたら LP を入れる
    static constexpr float kTransientRatio = 4.0f;
    static constexpr float kUnvoicedVoicing = 0.4f;

    PitchFormantShifter();
    ~PitchFormantShifter();

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** ブロック先頭で 1 回だけ設定する。範囲外はクランプされる。 */
    void setPitchSemitones (float semitones) noexcept;
    void setFormantSemitones (float semitones) noexcept;

    /** 0 = 遅延したドライのみ、1 = PSOLA 出力のみ。内部で 20 ms 平滑する。
        ドライ経路も同じ LOOKAHEAD だけ遅延しているので、
        どの値でも遅延は変わらず、切替でクリックもしない。
        VoiceEngine は shifterActive に応じてここを 0/1 に振る。 */
    void setWetMix (float wet01) noexcept;

    /** input と output は別バッファでも同一でもよい。numSamples <= maxBlockSize。 */
    void process (const float* input, float* output, int numSamples) noexcept;

    /** 常に kLookaheadSamples。設定でもバイパスでも変わらない。 */
    int getLatencySamples() const noexcept { return kLookaheadSamples; }

    const PitchTracker& getPitchTracker() const noexcept { return tracker; }

private:
    /** 出力が確定できるところまでグレインを合成する。オーディオスレッド専用。 */
    void synthesiseGrains() noexcept;

    /** 窓テーブルを grain 行 1 に作る（前半 = 立上り、後半 = 立下り）。prepare から 1 回だけ。 */
    void buildWindowTables() noexcept;

    static constexpr int kWindowTableSize = 512;

    PitchTracker tracker;

    double sampleRate = 48000.0;

    juce::AudioBuffer<float> rings;      // 4 行: in48 / outAccum / winAccum / dry
    juce::AudioBuffer<float> grain;      // 2 行: 窓掛けグレイン / リサンプル用の窓そのもの
    juce::AudioBuffer<float> prevSeg;    // 相関の参照セグメント（8k 用と 48k 用）

    int64_t writePos48 = 0;    // in48 に書いた総数
    int64_t readPos48 = 0;     // すでに出力した総数
    int64_t nextEpoch = 0;     // 次の合成エポック（出力の絶対位置）
    int64_t outFrontier = 0;   // 確定済み出力の先端
    int64_t lastMark = 0;      // 前回の解析マーク（入力の絶対位置）

    float pitchTargetSt = 0.0f, formantTargetSt = 0.0f;
    float pitchSmoothedSt = 0.0f, formantSmoothedSt = 0.0f;
    float wetTarget = 1.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix;

    uint32_t jitterState = 0x12345678u;   // 無声ジッタ用の xorshift。rand() は使わない。

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFormantShifter)
};

} // namespace kvc
