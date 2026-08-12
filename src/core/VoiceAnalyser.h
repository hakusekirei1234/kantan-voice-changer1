// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

#include "core/RawRing.h"

namespace kvc
{

//==============================================================================
/** 声の特徴をテキストで言い当てる。

    ★これは学習済みモデルではない。「ギャル女声」「セクシー女声」のような様式的な
      分類は本来ラベル付きデータで学習した分類器が要るが、このアプリはオフライン・
      軽量が前提で学習パイプラインを持たない。そこで音響特徴量からの
      ルールベース判定にしている。位置づけは「音響的な目安」であって、
      特徴が明確なもの（低い男声・少女声）ほど当たり、様式的なもの
      （ギャル・セクシー）ほど「それっぽい」程度に留まる。

    使う特徴量:
      f0            基本周波数（自己相関）
      formantScale  F1/F2 から求めた声道長の指標。1.0≒平均男性 / 1.18≒平均女性 / 1.35≒子供
      hnr           自己相関のピーク高。高い=澄んだ声 / 低い=かすれ・息漏れ
      brightness    2 kHz 以上のエネルギー比。高い=明るい・華やか
      variation     区間内の f0 の標準偏差（半音）。高い=抑揚が大きい
      jitter        周期ごとの f0 の揺れ。高い=年配・かすれ
      mismatch      f0 から期待される formantScale との乖離。加工感の指標

    スレッド契約: 全てメッセージスレッド専用。オーディオスレッドには一切触れない
    （RawRing を書込カーソルの手前だけ読む）。
*/
class VoiceAnalyser
{
public:
    enum class Tone { masculine, feminine, neutral };

    struct Result
    {
        juce::String label;
        Tone  tone = Tone::neutral;
        float confidence = 0.0f;   ///< 0..1。1 位と 2 位の差から出す
        bool  valid = false;       ///< 一度も判定していなければ false
    };

    /** 解析フレームの間隔。これで tick() を呼ぶ想定。 */
    static constexpr int kTickIntervalMs = 50;

    /** 無音と判断するまでの保持時間。短すぎると語間のポーズで消える。 */
    static constexpr double kSilenceHoldSeconds = 0.35;

    /** 絶対的な無音の下限。これを下回れば環境ノイズ追従に関わらず無音扱い。
        ★-60 dB では部屋の暗騒音を拾って、誰も喋っていないのに判定が出た。
          通常の発話は -30〜-20 dB なので、-48 dB でも実用上は取りこぼさない。 */
    static constexpr float kAbsoluteSilenceDb = -48.0f;

    /** フレームを「声」と認める周期性の下限（CMNDF から求めた HNR）。
        暗騒音やエアコンは周期性が無いのでここで落ちる。 */
    static constexpr float kMinVoicedHnr = 0.55f;

    /** 区間内でこの割合以上のフレームが有声でなければ判定しない。
        たまたま数フレーム周期性を持った雑音で判定が出るのを防ぐ。 */
    static constexpr float kMinVoicedRatio = 0.55f;

    /** 環境ノイズフロアからこれだけ上回ったら「声が出ている」とみなす。 */
    static constexpr float kSpeechMarginDb = 12.0f;

    explicit VoiceAnalyser (const RawRing& source);

    void prepare (double sampleRate);
    void reset();

    /** kTickIntervalMs ごとに呼ぶ。判定が更新されたら true。 */
    bool tick (double updateIntervalSeconds);

    /** 直近の判定に使った特徴量。詳細設定の情報タブと、判定のデバッグ用。
        「なぜこの判定になったか」を数値で見られないと調整のしようがない。 */
    struct Measured
    {
        float f0 = 0.0f, formantScale = 1.0f, hnr = 0.0f,
              brightness = 0.0f, variation = 0.0f, jitter = 0.0f, mismatch = 0.0f;
    };

    Measured getMeasured() const noexcept   { return measured; }
    Result getResult() const noexcept       { return result; }
    bool   isSpeaking() const noexcept      { return speaking; }
    float  getLevelDb() const noexcept      { return levelDb; }
    float  getNoiseFloorDb() const noexcept { return noiseFloorDb; }

private:
    struct Features
    {
        float f0 = 0.0f;
        float formantScale = 1.0f;
        float hnr = 0.0f;
        float brightness = 0.0f;
        bool  voiced = false;
    };

    /** 直近の窓を取り出して 1 フレームぶんの特徴量を出す。 */
    bool analyseFrame (Features&);

    /** 蓄積した特徴量から 1 つ選ぶ。 */
    void classify();

    const RawRing& source;

    double sampleRate = 48000.0;

    // 解析は 12 kHz へ落として行う。F1/F2 は 3 kHz 以下なので十分で、
    // LPC の条件数も良くなる（教科書どおりの 12 kHz / 次数 12〜14）。
    int    decimation = 4;
    double analysisRate = 12000.0;

    std::vector<float> window;      // 48 kHz の生窓
    std::vector<float> decimated;   // 12 kHz
    std::vector<float> lpcWork;

    // 区間の蓄積
    std::vector<float> f0History;
    float sumFormantScale = 0.0f;
    float sumHnr = 0.0f;
    float sumBrightness = 0.0f;
    int   voicedFrames = 0;
    double accumulatedSeconds = 0.0;

    // 無音判定
    bool   speaking = false;
    double silenceSeconds = 0.0;
    float  levelDb = -100.0f;
    float  noiseFloorDb = -70.0f;

    // 明るさ用の 2 次ハイパス状態
    float hpX1 = 0.0f, hpX2 = 0.0f, hpY1 = 0.0f, hpY2 = 0.0f;
    float hpB0 = 1.0f, hpB1 = 0.0f, hpB2 = 0.0f, hpA1 = 0.0f, hpA2 = 0.0f;

    // デシメーション用の 2 次ローパス状態
    float lpX1 = 0.0f, lpX2 = 0.0f, lpY1 = 0.0f, lpY2 = 0.0f;
    float lpB0 = 1.0f, lpB1 = 0.0f, lpB2 = 0.0f, lpA1 = 0.0f, lpA2 = 0.0f;

    Result result;
    Measured measured;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceAnalyser)
};

} // namespace kvc
