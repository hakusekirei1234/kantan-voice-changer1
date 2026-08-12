// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

namespace kvc
{

//==============================================================================
/** sqrt-Hann 解析/合成の WOLA フレーマ。

    PSOLA を採ったので、この STFT はノイズ除去専用の自前 STFT になる
    （ピッチシフタとは共有しない。DECISIONS.md 矛盾B）。

    - 解析窓・合成窓ともに sqrt-Hann。積が Hann になり HOP = N/4 で COLA を満たす。
    - 正規化は 2.0 で割る（75% オーバーラップの Hann の和）。
    - アルゴリズム遅延は N サンプル（出力サンプル t は、t から始まるフレームを
      処理し終えるまで確定しない）。getLatencySamples() が返すのはこの N。

    COLA 透過性の単体テストを必ず持たせること: インパルスを
    「何もしない FrameProcessor」で通し、入出力の差が -100 dB 以内であることを
    確認する。ここが崩れているとノイズ除去の OFF がビット透過にならず、
    「切っても音が変わる」という追いにくい不具合になる。

    スレッド契約:
      prepare / reset ... メッセージスレッド専用（確保する）
      process         ... オーディオスレッド専用
      FrameProcessor::processSpectrum は process() から同期的に呼ばれる。
      つまりオーディオスレッド上で走る。仮想呼び出しは許容するが、
      その中で確保・ロック・ログは禁止。
*/
class Wola
{
public:
    /** 1 フレームぶんのスペクトルを受け取るコールバック。 */
    class FrameProcessor
    {
    public:
        virtual ~FrameProcessor() = default;

        /** @param complexBins  re,im 交互配置。長さ 2*numBins フロート。
            @param numBins      N/2 + 1（N=256 なら 129）。
            オーディオスレッド専用。確保・ロック・ログ・例外は禁止。 */
        virtual void processSpectrum (float* complexBins, int numBins) noexcept = 0;
    };

    /** 何もしない実装。COLA 透過性テストとノイズ除去 OFF 時の検証に使う。 */
    class PassThroughProcessor final : public FrameProcessor
    {
    public:
        void processSpectrum (float*, int) noexcept override {}
    };

    Wola();
    ~Wola();

    /** @param fftSize      2 のべき乗。256 または 512。
        @param hopSize      fftSize / 4 であること（COLA の前提）。
        @param maxBlockSize 一度に process() に渡す最大サンプル数。 */
    void prepare (int fftSize, int hopSize, int maxBlockSize);

    void reset() noexcept;

    /** input と output は同一バッファでも構わない。 */
    void process (const float* input, float* output, int numSamples, FrameProcessor& proc) noexcept;

    int getFftSize() const noexcept { return fftSize; }
    int getHopSize() const noexcept { return hopSize; }
    int getNumBins() const noexcept { return fftSize / 2 + 1; }

    /** N サンプル。 */
    int getLatencySamples() const noexcept { return fftSize; }

    bool isPrepared() const noexcept { return fft != nullptr; }

    /** オフラインの COLA 自己テスト。インパルスを通して最大誤差 (dB) を返す。
        -100 dB 以下であることを単体テストで assert すること。 */
    static float measureColaErrorDb (int fftSize, int hopSize);

private:
    std::unique_ptr<juce::dsp::FFT> fft;

    int fftSize = 0;
    int hopSize = 0;
    int maxBlock = 0;

    juce::AudioBuffer<float> window;      // 1 行: sqrt-Hann
    juce::AudioBuffer<float> inputRing;   // fftSize + maxBlock
    juce::AudioBuffer<float> outputRing;
    juce::AudioBuffer<float> fftScratch;  // 2 * fftSize フロート

    int  ringWrite = 0;
    int  samplesUntilNextFrame = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Wola)
};

} // namespace kvc
