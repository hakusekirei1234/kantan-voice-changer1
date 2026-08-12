// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "core/ExternalProcessor.h"
#include "core/NoiseSuppressor.h"
#include "core/Parameters.h"
#include "core/PitchFormantShifter.h"

#include <algorithm>
#include <cmath>

namespace kvc
{

//==============================================================================
/** モニターバス最終段のソフトリミッタ。ヘッダオンリー。

    非技術者がループバックデバイスを入力に選ぶと、ヘッドホンに全音量の
    ハウリングが返る。これは聴覚障害のリスクがある実害なので、
    構成検証をすり抜けた場合の最後の防壁として必ず入れる。
    送信バスには入れない（相手側で二重に潰れるため）。

    スレッド契約: prepare はメッセージスレッド、process はオーディオスレッド。
*/
class SoftLimiter
{
public:
    static constexpr float kThreshold = 0.891f;      // -1 dBFS
    static constexpr float kCeiling = 0.99f;
    static constexpr float kAttackMs = 1.0f;
    static constexpr float kReleaseMs = 120.0f;

    void prepare (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        attackCoef  = 1.0f - std::exp (static_cast<float> (-1000.0 / (kAttackMs  * sr)));
        releaseCoef = 1.0f - std::exp (static_cast<float> (-1000.0 / (kReleaseMs * sr)));
        gain = 1.0f;
    }

    void reset() noexcept { gain = 1.0f; }

    void process (float* const* channels, int numChannels, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float peak = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                peak = std::max (peak, std::fabs (channels[c][i]));

            const float target = peak > kThreshold ? (kThreshold / std::max (peak, 1.0e-9f)) : 1.0f;
            const float coefficient = target < gain ? attackCoef : releaseCoef;
            gain += coefficient * (target - gain);

            for (int c = 0; c < numChannels; ++c)
                channels[c][i] = std::max (-kCeiling, std::min (kCeiling, channels[c][i] * gain));
        }
    }

private:
    float gain = 1.0f;
    float attackCoef = 0.5f;
    float releaseCoef = 0.01f;
};

//==============================================================================
/** DSP チェーンの束ね役。

        入力ゲイン → HPF 75 Hz + DC ブロック → NoiseSuppressor
                   → PitchFormantShifter → 出力トリム

    デバイスも juce::AudioProcessor も一切知らない純粋クラス。
    後から VST3 化するときと、WAV in / WAV out のオフラインテストのため。

    遅延はノイズ除去の N + PSOLA の 512 で固定。パラメータやバイパスで変わらない。
    既定（N=256）で 768 サンプル = 16.0 ms。

    ★ミュートとビープはここには無い。
      ミュートは送信バスへの書き込み側で、ビープはモニターバス側で、それぞれ
      AudioEngine が構造的に扱う。ここに入れるとバスごとの保証が壊れる。

    スレッド契約:
      prepare / reset ... メッセージスレッド専用（確保する）
      process         ... オーディオスレッド専用。先頭で ScopedNoDenormals を張ること。
*/
class VoiceEngine
{
public:
    struct Config
    {
        double sampleRate = kPreferredSampleRate;
        int    maxBlockSize = 512;
        NoiseQuality noiseQuality = NoiseQuality::lowLatency256;

        /** 内蔵 PSOLA の代わりに使う外部プラグイン。nullptr なら内蔵。
            ★prepare より前に決まっていなければならない。差し替えは必ずエンジンを
              止めてから行う（遅延が変わるため実行中の交換は許されない）。 */
        ExternalProcessor* external = nullptr;

        /** ノイズ除去を信号経路に置くか。
            OFF のときに経路から外すと STFT の N ぶん（既定 256 = 5.3 ms）遅延が減る。
            noiseQuality と同じく prepare 時にしか効かない値なので、
            切り替えたら AudioEngine::restartAsync() でグラフを作り直すこと。 */
        bool noiseSuppressionInPath = true;
    };

    static constexpr float kHighPassHz = 75.0f;
    static constexpr float kHighPassQ = 0.7071f;

    /** ゲイン・ミュート系は Linear 10 ms、ピッチ比は Multiplicative 50 ms、
        ノイズ除去の ON/OFF は NoiseSuppressor 内部で 40 ms クロスフェード。
        bool のハード切替は禁止。 */
    static constexpr double kGainSmoothSeconds = 0.010;
    static constexpr double kRatioSmoothSeconds = 0.050;

    VoiceEngine();
    ~VoiceEngine();

    void prepare (const Config&);
    void reset() noexcept;

    /** モノラル in / モノラル out。input == output でも可。
        params はブロック先頭で 1 回だけ取ったスナップショットを渡すこと。 */
    void process (const float* input, float* output, int numSamples,
                  const VoiceParamSnapshot& params) noexcept;

    /** ノイズ除去 N + PSOLA 512。実行中は絶対に変化しない。 */
    int getLatencySamples() const noexcept { return latencySamples; }

    const Config& getConfig() const noexcept { return config; }

private:
    Config config;
    int latencySamples = kPsolaLookaheadSamples + kNoiseFftLowLatency;

    juce::dsp::IIR::Filter<float> highPass;
    NoiseSuppressor noiseSuppressor;
    PitchFormantShifter shifter;

    juce::AudioBuffer<float> scratch;   // 1 行、maxBlockSize

    // 外部プラグイン使用時のドライ遅延。バイパスで遅延が動かないようにするためだけの物。
    juce::AudioBuffer<float> dryDelay;
    int dryWritePos = 0;
    int dryDelaySamples = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> inputGain, outputGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceEngine)
};

} // namespace kvc
