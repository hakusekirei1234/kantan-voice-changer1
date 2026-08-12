// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/Wola.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace kvc
{

//==============================================================================
Wola::Wola() = default;
Wola::~Wola() = default;

//==============================================================================
void Wola::prepare (int newFftSize, int newHopSize, int newMaxBlockSize)
{
    jassert (juce::isPowerOfTwo (newFftSize));

    // HOP = N/4 以外は許さない。sqrt-Hann の積（= 周期 Hann）が 4 重なりで
    // ちょうど 2.0 になる配置だけが、この実装の正規化定数と整合する。
    jassert (newHopSize == newFftSize / 4);

    fftSize  = newFftSize;
    hopSize  = newHopSize;
    maxBlock = juce::jmax (1, newMaxBlockSize);

    fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) fftSize)));

    window.setSize (1, fftSize);
    {
        auto* w = window.getWritePointer (0);

        for (int n = 0; n < fftSize; ++n)
        {
            // 分母は N-1 ではなく N（周期 Hann）。対称 Hann にすると COLA が崩れ、
            // インパルス透過誤差が -100 dB を超える。
            const double hann = 0.5 * (1.0 - std::cos (2.0 * juce::MathConstants<double>::pi
                                                           * (double) n / (double) fftSize));
            w[n] = (float) std::sqrt (hann);
        }
    }

    // ブロック 1 回ぶんを末尾に足してから前詰めするので、履歴 N + 最大ブロック。
    inputRing.setSize (1, fftSize + maxBlock);

    // OLA 累算器。1 フレームは「これから出す N サンプル」にちょうど収まるので N で足りる
    // （出力を先に取り出してから加算する順序にしてあるため。process() のコメント参照）。
    outputRing.setSize (1, fftSize);

    fftScratch.setSize (1, 2 * fftSize);

    reset();

   #if JUCE_DEBUG
    // 自己テストが内部で prepare() を呼ぶので、再入したら 2 周目で止める。
    static std::atomic<bool> selfTestRunning { false };

    if (! selfTestRunning.exchange (true))
    {
        const float errorDb = measureColaErrorDb (fftSize, hopSize);
        jassert (errorDb <= -100.0f);
        juce::ignoreUnused (errorDb);
        selfTestRunning.store (false);
    }
   #endif
}

void Wola::reset() noexcept
{
    inputRing.clear();
    outputRing.clear();
    fftScratch.clear();

    ringWrite = 0;
    samplesUntilNextFrame = juce::jmax (1, hopSize);
}

//==============================================================================
void Wola::process (const float* input, float* output, int numSamples, FrameProcessor& proc) noexcept
{
    if (numSamples <= 0)
        return;

    if (fft == nullptr)
    {
        if (output != input)
            juce::FloatVectorOperations::copy (output, input, numSamples);

        return;
    }

    auto* history = inputRing.getWritePointer (0);
    auto* accum   = outputRing.getWritePointer (0);
    auto* scratch = fftScratch.getWritePointer (0);
    const auto* win = window.getReadPointer (0);

    const int   bins     = fftSize / 2 + 1;
    const int   nyquist  = 2 * (fftSize / 2) + 1;
    const float olaScale = 2.0f * (float) hopSize / (float) fftSize;   // 周期 Hann の 4 重なり和 = 2.0

    const float* in  = input;
    float*       out = output;
    int          remaining = numSamples;

    while (remaining > 0)
    {
        const int numThisTime = juce::jmin (remaining, maxBlock);

        // 入力を先に退避する。input == output のエイリアスを許す契約なので、
        // 出力を書き始める前にコピーを終えていなければならない。
        juce::FloatVectorOperations::copy (history + fftSize, in, numThisTime);

        for (int i = 0; i < numThisTime; ++i)
        {
            out[i] = accum[ringWrite];
            accum[ringWrite] = 0.0f;

            if (++ringWrite == fftSize)
                ringWrite = 0;

            if (--samplesUntilNextFrame > 0)
                continue;

            samplesUntilNextFrame = hopSize;

            // このサンプルを最後尾に含む直近 N サンプル。
            const float* frame = history + i + 1;

            for (int k = 0; k < fftSize; ++k)
                scratch[k] = frame[k] * win[k];

            juce::FloatVectorOperations::clear (scratch + fftSize, fftSize);

            fft->performRealOnlyForwardTransform (scratch, true);

            proc.processSpectrum (scratch, bins);

            // 実信号の逆変換は DC と Nyquist が実数であることを前提にしている。
            // FrameProcessor が実数ゲイン以外を書いても壊れないようにここで潰す。
            scratch[1] = 0.0f;
            scratch[nyquist] = 0.0f;

            fft->performRealOnlyInverseTransform (scratch);

            // 出力を取り出したあとの ringWrite が、このフレームの先頭サンプルの位置に一致する。
            // この順序のおかげで累算器は N サンプルで足り、遅延がちょうど N になる。
            int w = ringWrite;

            for (int k = 0; k < fftSize; ++k)
            {
                accum[w] += scratch[k] * win[k] * olaScale;

                if (++w == fftSize)
                    w = 0;
            }
        }

        // 常に history[0 .. N-1] が「直近 N サンプル」になるよう前詰めする。
        std::memmove (history, history + numThisTime, sizeof (float) * (size_t) fftSize);

        in        += numThisTime;
        out       += numThisTime;
        remaining -= numThisTime;
    }
}

//==============================================================================
float Wola::measureColaErrorDb (int fftSizeToTest, int hopSizeToTest)
{
    // オフライン専用。確保してよい。
    Wola wola;
    const int block = juce::jmax (1, hopSizeToTest);
    wola.prepare (fftSizeToTest, hopSizeToTest, block);

    PassThroughProcessor passThrough;

    const int latency = fftSizeToTest;               // 設計上の遅延
    const int total   = latency + 4 * fftSizeToTest; // OLA が閉じ切るだけの尾を足す

    juce::AudioBuffer<float> buffer (1, total);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);

    auto* data = buffer.getWritePointer (0);

    for (int pos = 0; pos < total; pos += block)
        wola.process (data + pos, data + pos, juce::jmin (block, total - pos), passThrough);

    float maxError = 0.0f;

    for (int n = 0; n < total; ++n)
    {
        const float expected = (n == latency) ? 1.0f : 0.0f;
        maxError = juce::jmax (maxError, std::abs (data[n] - expected));
    }

    return maxError > 0.0f ? 20.0f * std::log10 (maxError) : -1000.0f;
}

} // namespace kvc
