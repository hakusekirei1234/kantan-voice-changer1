// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/VoiceEngine.h"

#include <cmath>

namespace kvc
{

//==============================================================================
VoiceEngine::VoiceEngine() = default;
VoiceEngine::~VoiceEngine() = default;

//==============================================================================
void VoiceEngine::prepare (const Config& newConfig)
{
    config = newConfig;

    if (config.sampleRate <= 0.0)
        config.sampleRate = kPreferredSampleRate;

    config.maxBlockSize = juce::jlimit (1, kMaxBlockSize, config.maxBlockSize);

    const int fftSize = fftSizeForQuality (config.noiseQuality);
    const int hopSize = hopSizeForQuality (config.noiseQuality);

    // 遅延は「サンプル数」で固定される量。レートが変わると ms は変わるが
    // 先読み 512 も STFT の N も サンプル単位の定数なので本数は変わらない。
    // 外部プラグイン使用時は PSOLA の代わりにプラグインの申告値が乗る（後で確定する）。
    const int noiseLatency = config.noiseSuppressionInPath ? fftSize : 0;

    latencySamples = kPsolaLookaheadSamples + noiseLatency;

    // 2 次バタワース HPF 75 Hz。DC はここで一緒に落ちる（DC で -inf dB）ので
    // 前段に別の DC ブロッカは置かない。
    highPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (
                                config.sampleRate, kHighPassHz, kHighPassQ);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = config.sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (config.maxBlockSize);
    spec.numChannels      = 1;
    highPass.prepare (spec);
    highPass.reset();

    // 経路から外すときも prepare だけはしておく（再度 ON にしたとき、
    // 未初期化のまま process へ入らないように）。遅延計算には入れない。
    noiseSuppressor.prepare (config.sampleRate, fftSize, hopSize, config.maxBlockSize);
    shifter.prepare (config.sampleRate, config.maxBlockSize);

    // process() は確保できないので、ここで最大ブロック長ぶん確保しきる。
    scratch.setSize (1, config.maxBlockSize, false, true, false);
    scratch.clear();

    inputGain .reset (config.sampleRate, kGainSmoothSeconds);
    outputGain.reset (config.sampleRate, kGainSmoothSeconds);
    wetMix    .reset (config.sampleRate, kRatioSmoothSeconds);

    inputGain .setCurrentAndTargetValue (1.0f);
    outputGain.setCurrentAndTargetValue (1.0f);
    wetMix    .setCurrentAndTargetValue (0.0f);

    if (config.external != nullptr)
    {
        config.external->prepare (config.sampleRate, config.maxBlockSize);

        const int pluginLatency = juce::jmax (0, config.external->getLatencySamples());

        latencySamples = noiseLatency + pluginLatency;

        // バイパス（声を変えるを OFF）でも遅延を動かさないため、ドライ側を
        // プラグインと同じだけ遅らせる。内蔵 PSOLA は自前で同じことをしている。
        dryDelaySamples = pluginLatency;
        dryDelay.setSize (1, juce::jmax (1, pluginLatency + config.maxBlockSize + 8), false, true, false);
        dryDelay.clear();
        dryWritePos = 0;
    }
    else
    {
        dryDelaySamples = 0;
        dryDelay.setSize (1, 1, false, true, false);
        dryDelay.clear();
        dryWritePos = 0;

        // 要件4。ここが崩れると送信/モニターの遅延補正が嘘になる。
        jassert (latencySamples == shifter.getLatencySamples() + noiseLatency);
    }
}

//==============================================================================
void VoiceEngine::reset() noexcept
{
    highPass.reset();
    noiseSuppressor.reset();
    shifter.reset();
    scratch.clear();
    dryDelay.clear();
    dryWritePos = 0;

    if (config.external != nullptr)
        config.external->reset();

    // ストリーム再開時は連続性が無いので、平滑器はランプさせずに現在値へ飛ばす。
    inputGain .setCurrentAndTargetValue (inputGain .getTargetValue());
    outputGain.setCurrentAndTargetValue (outputGain.getTargetValue());
    wetMix    .setCurrentAndTargetValue (wetMix    .getTargetValue());
}

//==============================================================================
void VoiceEngine::process (const float* input, float* output, int numSamples,
                           const VoiceParamSnapshot& params) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    if (input == nullptr || output == nullptr || numSamples <= 0)
        return;

    const int capacity = scratch.getNumSamples();
    jassert (numSamples <= capacity);

    // prepare より大きいブロックが来ても確保はできない。溢れた分は無音にする。
    const int n = juce::jmin (numSamples, capacity);

    for (int i = n; i < numSamples; ++i)
        output[i] = 0.0f;

    if (n <= 0)
        return;

    float* const work = scratch.getWritePointer (0);

    //--------------------------------------------------------------------------
    // 1. 入力ゲイン
    inputGain.setTargetValue (params.inputGainLinear);

    if (inputGain.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
            work[i] = input[i] * inputGain.getNextValue();
    }
    else
    {
        const float g = inputGain.getCurrentValue();

        for (int i = 0; i < n; ++i)
            work[i] = input[i] * g;
    }

    //--------------------------------------------------------------------------
    // 2. HPF 75 Hz + DC ブロック
    for (int i = 0; i < n; ++i)
        work[i] = highPass.processSample (work[i]);

    highPass.snapToZero();

    //--------------------------------------------------------------------------
    // 3. ノイズ除去。ON/OFF は内部の 40 ms ミックスランプで、bool のハード切替はしない。
    //    OFF 中も推定器は回り続ける（バイパスすると再有効化時に学習し直しになる）。
    if (config.noiseSuppressionInPath)
    {
        noiseSuppressor.setEnabled (params.noiseSuppressionEnabled);
        noiseSuppressor.setStrength (params.noiseStrength);
        noiseSuppressor.process (work, work, n);
    }

    //--------------------------------------------------------------------------
    // 4. TD-PSOLA。半音値の平滑（20 ms 一次遅れ + 1 半音/グレインのスルーレート制限）は
    //    シフタ側が持つ。半音の線形ランプ = 比の乗算的ランプなので二重に掛けない。
    wetMix.setTargetValue (params.shifterActive ? 1.0f : 0.0f);

    if (config.external != nullptr)
    {
        // 外部プラグイン経路。プラグインは自前でバイパスを持たないので、
        // ドライをプラグインと同じだけ遅らせて、ここで混ぜる。
        float* const dry = dryDelay.getWritePointer (0);
        const int dryCapacity = dryDelay.getNumSamples();

        for (int i = 0; i < n; ++i)
            dry[(dryWritePos + i) % dryCapacity] = work[i];

        config.external->processMono (work, n);

        for (int i = 0; i < n; ++i)
        {
            const int readPos = ((dryWritePos + i - dryDelaySamples) % dryCapacity + dryCapacity) % dryCapacity;
            const float d = dry[readPos];
            const float m = wetMix.getNextValue();
            work[i] = d + m * (work[i] - d);
        }

        dryWritePos = (dryWritePos + n) % dryCapacity;
    }
    else
    {
        shifter.setPitchSemitones (params.pitchSemitones);
        shifter.setFormantSemitones (params.formantSemitones);

        // バイパスもドライ側を同じ 512 サンプル遅延させたウェット/ドライで行う。
        // 遅延が動かないので切替でプチッと鳴らない。
        wetMix.skip (n);
        shifter.setWetMix (wetMix.getCurrentValue());

        shifter.process (work, work, n);
    }

    //--------------------------------------------------------------------------
    // 5. 出力トリム
    outputGain.setTargetValue (params.outputGainLinear);

    bool sawNonFinite = false;

    if (outputGain.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = work[i] * outputGain.getNextValue();
            sawNonFinite |= ! std::isfinite (v);
            output[i] = std::isfinite (v) ? v : 0.0f;
        }
    }
    else
    {
        const float g = outputGain.getCurrentValue();

        for (int i = 0; i < n; ++i)
        {
            const float v = work[i] * g;
            sawNonFinite |= ! std::isfinite (v);
            output[i] = std::isfinite (v) ? v : 0.0f;
        }
    }

    // NaN が IIR の状態に入ると以後ずっと無音になる。自力で復帰させる。
    // （reset() は確保しないのでオーディオスレッドから呼んでよいのはこの 1 つだけ）
    if (sawNonFinite)
        highPass.reset();
}

} // namespace kvc
