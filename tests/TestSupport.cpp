// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "TestSupport.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>

namespace kvctest
{

//==============================================================================
std::vector<TestCase>& registry()
{
    static std::vector<TestCase> cases;
    return cases;
}

void reportCheck (bool ok, const std::string& message)
{
    if (! ok)
        throw Failure { message };
}

void checkNear (double actual, double expected, double relTolerance,
                const char* what, const char* file, int line)
{
    const double denom = std::abs (expected) > 1.0e-12 ? std::abs (expected) : 1.0;
    const double error = std::abs (actual - expected) / denom;

    if (error > relTolerance)
    {
        char buffer[512];
        std::snprintf (buffer, sizeof (buffer),
                       "NEAR failed: %s = %.4f, expected %.4f (rel err %.4f > %.4f)  [%s:%d]",
                       what, actual, expected, error, relTolerance, file, line);
        throw Failure { buffer };
    }
}

void checkInRange (double actual, double low, double high,
                   const char* what, const char* file, int line)
{
    if (! (actual >= low && actual <= high))
    {
        char buffer[512];
        std::snprintf (buffer, sizeof (buffer),
                       "RANGE failed: %s = %.4f, expected within [%.4f, %.4f]  [%s:%d]",
                       what, actual, low, high, file, line);
        throw Failure { buffer };
    }
}

void note (const std::string& text)
{
    std::printf ("      . %s\n", text.c_str());
    std::fflush (stdout);
}

//==============================================================================
namespace
{
    /** 2 極共振器。フォルマント 1 本ぶん。共振周波数での利得を 1 に正規化する。 */
    struct Resonator
    {
        double a1 = 0.0, a2 = 0.0, gain = 1.0;
        double y1 = 0.0, y2 = 0.0;

        void set (double frequency, double bandwidth, double sampleRate)
        {
            const double r = std::exp (-juce::MathConstants<double>::pi * bandwidth / sampleRate);
            const double theta = 2.0 * juce::MathConstants<double>::pi * frequency / sampleRate;

            a1 = 2.0 * r * std::cos (theta);
            a2 = -r * r;

            // 段を重ねても発散しないように、共振点の利得で割っておく。
            const double cosT = std::cos (theta), sinT = std::sin (theta);
            const double reH = 1.0 - a1 * cosT - a2 * std::cos (2.0 * theta);
            const double imH = a1 * sinT + a2 * std::sin (2.0 * theta);
            gain = std::sqrt (reH * reH + imH * imH);
        }

        double process (double x)
        {
            const double y = gain * x + a1 * y1 + a2 * y2;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    inline unsigned int xorshift (unsigned int& state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    inline double uniform (unsigned int& state)
    {
        return static_cast<double> (xorshift (state)) / 4294967296.0;
    }
}

//==============================================================================
std::vector<float> makeVowel (double sampleRate, double f0, double seconds,
                              const std::vector<Formant>& formants,
                              double jitterPercent)
{
    const int numSamples = static_cast<int> (sampleRate * seconds);
    std::vector<float> out (static_cast<size_t> (numSamples), 0.0f);

    std::vector<Resonator> resonators (formants.size());

    for (size_t i = 0; i < formants.size(); ++i)
        resonators[i].set (formants[i].frequency, formants[i].bandwidth, sampleRate);

    unsigned int state = 0x9E3779B9u;

    const double basePeriod = sampleRate / f0;

    // 声門波（Rosenberg 型）。開放 40% / 閉鎖 16%。
    std::vector<double> source (static_cast<size_t> (numSamples), 0.0);

    double nextPulse = 0.0;
    double period = basePeriod;
    int pulseStart = 0;

    while (pulseStart < numSamples)
    {
        const int open  = juce::jmax (2, static_cast<int> (0.40 * period));
        const int close = juce::jmax (1, static_cast<int> (0.16 * period));

        for (int i = 0; i < open && pulseStart + i < numSamples; ++i)
        {
            const double t = static_cast<double> (i) / static_cast<double> (open);
            source[static_cast<size_t> (pulseStart + i)]
                += 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * t));
        }

        for (int i = 0; i < close && pulseStart + open + i < numSamples; ++i)
        {
            const double t = static_cast<double> (i) / static_cast<double> (close);
            source[static_cast<size_t> (pulseStart + open + i)]
                += std::cos (0.5 * juce::MathConstants<double>::pi * t);
        }

        const double jitter = jitterPercent * 0.01 * (uniform (state) * 2.0 - 1.0);
        period = basePeriod * (1.0 + jitter);

        nextPulse += period;
        pulseStart = static_cast<int> (nextPulse);
    }

    // 声門波は積分形なので、1 次差分で口唇放射（+6 dB/oct）を入れる。
    double previous = 0.0;

    for (int n = 0; n < numSamples; ++n)
    {
        double x = source[static_cast<size_t> (n)] - previous;
        previous = source[static_cast<size_t> (n)];

        for (auto& r : resonators)
            x = r.process (x);

        out[static_cast<size_t> (n)] = static_cast<float> (x);
    }

    // ピーク 0.5 に正規化。クリップもせず、リミッタの閾値にも触れない水準。
    float maxAbs = 0.0f;

    for (float v : out)
        maxAbs = juce::jmax (maxAbs, std::abs (v));

    if (maxAbs > 1.0e-9f)
    {
        const float scale = 0.5f / maxAbs;

        for (float& v : out)
            v *= scale;
    }

    return out;
}

//==============================================================================
std::vector<float> makeNoise (int numSamples, float amplitude, unsigned int seed)
{
    std::vector<float> out (static_cast<size_t> (juce::jmax (0, numSamples)), 0.0f);
    unsigned int state = seed != 0u ? seed : 1u;

    for (float& v : out)
        v = amplitude * static_cast<float> (uniform (state) * 2.0 - 1.0);

    return out;
}

//==============================================================================
double estimateF0 (const float* data, int numSamples, double sampleRate,
                   double minHz, double maxHz)
{
    const int minLag = juce::jmax (2, static_cast<int> (sampleRate / maxHz));
    const int maxLag = static_cast<int> (sampleRate / minHz);
    const int window = 2 * maxLag;

    if (numSamples < window + maxLag)
        return 0.0;

    // 窓を 8 個ぶんずらして測り、中央値を採る。1 窓だとオクターブ誤りが素通りする。
    std::vector<double> estimates;

    const int numWindows = 8;
    const int span = numSamples - window - maxLag;
    const int step = juce::jmax (1, span / numWindows);

    std::vector<double> diff (static_cast<size_t> (maxLag + 1), 0.0);
    std::vector<double> cmndf (static_cast<size_t> (maxLag + 1), 0.0);

    for (int w = 0; w < numWindows; ++w)
    {
        const int offset = w * step;

        if (offset + window + maxLag > numSamples)
            break;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double sum = 0.0;

            for (int i = 0; i < window; ++i)
            {
                const double d = static_cast<double> (data[offset + i])
                                     - static_cast<double> (data[offset + i + lag]);
                sum += d * d;
            }

            diff[static_cast<size_t> (lag)] = sum;
        }

        double running = 0.0;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            running += diff[static_cast<size_t> (lag)];
            const double mean = running / static_cast<double> (lag - minLag + 1);
            cmndf[static_cast<size_t> (lag)] = mean > 1.0e-15
                                                   ? diff[static_cast<size_t> (lag)] / mean
                                                   : 1.0;
        }

        // YIN の絶対閾値。最小値ではなく「最初に 0.15 を下回った谷」を採る
        // （最小値を採るとオクターブ下を掴むことがある）。
        int best = -1;

        for (int lag = minLag + 1; lag < maxLag; ++lag)
        {
            if (cmndf[static_cast<size_t> (lag)] < 0.15
                && cmndf[static_cast<size_t> (lag)] <= cmndf[static_cast<size_t> (lag + 1)])
            {
                best = lag;
                break;
            }
        }

        if (best < 0)
        {
            double lowest = 1.0e18;

            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                if (cmndf[static_cast<size_t> (lag)] < lowest)
                {
                    lowest = cmndf[static_cast<size_t> (lag)];
                    best = lag;
                }
            }

            if (lowest > 0.5)
                continue;   // 有声とみなせない窓は捨てる
        }

        // 放物線補間。1 サンプル格子のままだと高い F0 で 1% 単位の量子化誤差が出る。
        double refined = static_cast<double> (best);

        if (best > minLag && best < maxLag)
        {
            const double a = cmndf[static_cast<size_t> (best - 1)];
            const double b = cmndf[static_cast<size_t> (best)];
            const double c = cmndf[static_cast<size_t> (best + 1)];
            const double denom = 2.0 * (2.0 * b - a - c);

            if (std::abs (denom) > 1.0e-12)
                refined += (c - a) / denom;
        }

        estimates.push_back (sampleRate / refined);
    }

    if (estimates.empty())
        return 0.0;

    std::sort (estimates.begin(), estimates.end());

    return estimates[estimates.size() / 2];
}

//==============================================================================
namespace
{
    /** ハン窓つき FFT の振幅スペクトルを、50% ホップの全フレームで平均する。 */
    std::vector<double> averageMagnitudeSpectrum (const float* data, int numSamples,
                                                  int fftOrder, int& fftSizeOut)
    {
        const int fftSize = 1 << fftOrder;
        fftSizeOut = 0;

        if (numSamples < fftSize)
            return {};

        juce::dsp::FFT fft (fftOrder);

        std::vector<double> magnitude (static_cast<size_t> (fftSize / 2), 0.0);
        std::vector<float> scratch (static_cast<size_t> (fftSize * 2), 0.0f);

        const int hop = fftSize / 2;
        int frames = 0;

        for (int offset = 0; offset + fftSize <= numSamples; offset += hop)
        {
            std::fill (scratch.begin(), scratch.end(), 0.0f);

            for (int i = 0; i < fftSize; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                            * static_cast<float> (i)
                                                            / static_cast<float> (fftSize));
                scratch[static_cast<size_t> (i)] = data[offset + i] * w;
            }

            fft.performFrequencyOnlyForwardTransform (scratch.data());

            for (int i = 0; i < fftSize / 2; ++i)
                magnitude[static_cast<size_t> (i)] += static_cast<double> (scratch[static_cast<size_t> (i)]);

            ++frames;
        }

        if (frames == 0)
            return {};

        for (double& m : magnitude)
            m /= static_cast<double> (frames);

        fftSizeOut = fftSize;
        return magnitude;
    }
}

double spectralCentroid (const float* data, int numSamples, double sampleRate,
                         double lowHz, double highHz)
{
    int fftSize = 0;
    const auto magnitude = averageMagnitudeSpectrum (data, numSamples, 12, fftSize);

    if (fftSize == 0)
        return 0.0;

    const double binHz = sampleRate / static_cast<double> (fftSize);
    const int lowBin  = juce::jmax (1, static_cast<int> (lowHz / binHz));
    const int highBin = juce::jmin (fftSize / 2 - 1, static_cast<int> (highHz / binHz));

    double weighted = 0.0, total = 0.0;

    for (int i = lowBin; i <= highBin; ++i)
    {
        const double m = magnitude[static_cast<size_t> (i)];
        weighted += m * static_cast<double> (i) * binHz;
        total += m;
    }

    return total > 1.0e-15 ? weighted / total : 0.0;
}

double envelopePeakHz (const float* data, int numSamples, double sampleRate,
                       double lowHz, double highHz)
{
    int fftSize = 0;
    const auto magnitude = averageMagnitudeSpectrum (data, numSamples, 12, fftSize);

    if (fftSize == 0)
        return 0.0;

    const double binHz = sampleRate / static_cast<double> (fftSize);

    // 倍音の櫛を均すための移動平均。±400 Hz は最低 F0 (60 Hz) の 6 倍以上あるので、
    // どの声でも倍音は消えてフォルマントの山だけが残る。
    const int smoothBins = juce::jmax (3, static_cast<int> (400.0 / binHz));
    const int numBins = static_cast<int> (magnitude.size());

    std::vector<double> smoothed (magnitude.size(), 0.0);

    for (int i = 0; i < numBins; ++i)
    {
        double sum = 0.0;
        int count = 0;

        for (int k = -smoothBins; k <= smoothBins; ++k)
        {
            const int j = i + k;

            if (j >= 0 && j < numBins)
            {
                sum += magnitude[static_cast<size_t> (j)];
                ++count;
            }
        }

        smoothed[static_cast<size_t> (i)] = count > 0 ? sum / static_cast<double> (count) : 0.0;
    }

    const int lowBin  = juce::jmax (1, static_cast<int> (lowHz / binHz));
    const int highBin = juce::jmin (numBins - 1, static_cast<int> (highHz / binHz));

    int bestBin = lowBin;

    for (int i = lowBin; i <= highBin; ++i)
        if (smoothed[static_cast<size_t> (i)] > smoothed[static_cast<size_t> (bestBin)])
            bestBin = i;

    return static_cast<double> (bestBin) * binHz;
}

namespace
{
    /** 倍音の櫛を均した振幅スペクトル。 */
    std::vector<double> smoothedSpectrum (const float* data, int numSamples, double sampleRate,
                                          int& fftSizeOut)
    {
        const auto magnitude = averageMagnitudeSpectrum (data, numSamples, 12, fftSizeOut);

        if (fftSizeOut == 0)
            return {};

        const double binHz = sampleRate / static_cast<double> (fftSizeOut);
        const int smoothBins = juce::jmax (3, static_cast<int> (400.0 / binHz));
        const int numBins = static_cast<int> (magnitude.size());

        std::vector<double> smoothed (magnitude.size(), 0.0);

        for (int i = 0; i < numBins; ++i)
        {
            double sum = 0.0;
            int count = 0;

            for (int k = -smoothBins; k <= smoothBins; ++k)
            {
                const int j = i + k;

                if (j >= 0 && j < numBins)
                {
                    sum += magnitude[static_cast<size_t> (j)];
                    ++count;
                }
            }

            smoothed[static_cast<size_t> (i)] = count > 0 ? sum / static_cast<double> (count) : 0.0;
        }

        return smoothed;
    }
}

double logSpectralDistanceDb (const float* a, int numSamplesA,
                              const float* b, int numSamplesB,
                              double sampleRate, double lowHz, double highHz)
{
    int sizeA = 0, sizeB = 0;
    const auto specA = smoothedSpectrum (a, numSamplesA, sampleRate, sizeA);
    const auto specB = smoothedSpectrum (b, numSamplesB, sampleRate, sizeB);

    if (sizeA == 0 || sizeB == 0 || sizeA != sizeB)
        return 1.0e9;

    const double binHz = sampleRate / static_cast<double> (sizeA);
    const int lowBin  = juce::jmax (1, static_cast<int> (lowHz / binHz));
    const int highBin = juce::jmin (static_cast<int> (specA.size()) - 1, static_cast<int> (highHz / binHz));

    // 平均レベルの差は形の違いではないので先に落とす。
    double meanA = 0.0, meanB = 0.0;

    for (int i = lowBin; i <= highBin; ++i)
    {
        meanA += 20.0 * std::log10 (juce::jmax (1.0e-12, specA[static_cast<size_t> (i)]));
        meanB += 20.0 * std::log10 (juce::jmax (1.0e-12, specB[static_cast<size_t> (i)]));
    }

    const double count = static_cast<double> (highBin - lowBin + 1);
    meanA /= count;
    meanB /= count;

    double sum = 0.0;

    for (int i = lowBin; i <= highBin; ++i)
    {
        const double da = 20.0 * std::log10 (juce::jmax (1.0e-12, specA[static_cast<size_t> (i)])) - meanA;
        const double db = 20.0 * std::log10 (juce::jmax (1.0e-12, specB[static_cast<size_t> (i)])) - meanB;

        sum += (da - db) * (da - db);
    }

    return std::sqrt (sum / count);
}

double amplitudeModulationDepth (const float* data, int numSamples, double sampleRate)
{
    const int frame = juce::jmax (8, static_cast<int> (0.005 * sampleRate));

    std::vector<double> levels;

    for (int offset = 0; offset + frame <= numSamples; offset += frame)
        levels.push_back (rms (data + offset, frame));

    if (levels.size() < 4)
        return 0.0;

    double mean = 0.0;

    for (double l : levels)
        mean += l;

    mean /= static_cast<double> (levels.size());

    if (mean < 1.0e-9)
        return 0.0;

    double variance = 0.0;

    for (double l : levels)
        variance += (l - mean) * (l - mean);

    variance /= static_cast<double> (levels.size());

    return std::sqrt (variance) / mean;
}

//==============================================================================
double rms (const float* data, int numSamples)
{
    if (numSamples <= 0)
        return 0.0;

    double sum = 0.0;

    for (int i = 0; i < numSamples; ++i)
        sum += static_cast<double> (data[i]) * static_cast<double> (data[i]);

    return std::sqrt (sum / static_cast<double> (numSamples));
}

double peak (const float* data, int numSamples)
{
    double p = 0.0;

    for (int i = 0; i < numSamples; ++i)
        p = juce::jmax (p, std::abs (static_cast<double> (data[i])));

    return p;
}

bool allFinite (const float* data, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
        if (! std::isfinite (data[i]))
            return false;

    return true;
}

} // namespace kvctest
