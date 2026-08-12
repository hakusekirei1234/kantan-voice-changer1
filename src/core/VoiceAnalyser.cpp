// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/VoiceAnalyser.h"

#include <algorithm>
#include <cmath>

namespace kvc
{

namespace
{
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    constexpr int kWindowSamples48 = 4096;    // 約 85 ms。2 周期以上を確実に含む
    constexpr int kLpcOrder = 14;

    constexpr float kMinF0 = 60.0f;
    constexpr float kMaxF0 = 500.0f;

    /** f0 から期待される声道長指標。実測の目安を直線で結んだだけの近似。
        男性 f0=120 → 1.00 / 女性 f0=210 → 1.19 / 子供 f0=290 → 1.35 */
    inline float expectedFormantScale (float f0) noexcept
    {
        return 0.753f + 0.00206f * f0;
    }

    //==========================================================================
    /** 判定に使う 7 次元。単位は下のプロファイル表と揃えてある。 */
    struct Dims
    {
        float f0 = 0.0f;            // Hz
        float formantScale = 1.0f;  // 0.85 .. 1.5
        float hnr = 0.0f;           // 0..1
        float brightness = 0.0f;    // 0..1
        float variation = 0.0f;     // 0..1（半音の標準偏差 / 4）
        float jitter = 0.0f;        // 0..1
        float mismatch = 0.0f;      // 0..1（加工感）
    };

    struct Profile
    {
        const char* label;
        VoiceAnalyser::Tone tone;
        float f0, fs, hnr, bright, var, jit, mis;
        float f0W, fsW;     // 0 なら既定幅。「この次元は問わない」を表現するために持つ
    };

    // 幅（許容の広さ）。狭くしすぎると何も当たらなくなる。
    constexpr float kwF0 = 42.0f;
    constexpr float kwFs = 0.13f;
    constexpr float kwOther = 0.30f;
    constexpr float kwMismatch = 0.35f;

    // ★次元ごとの効き。
    //   主要 2 次元（声の高さ・声道長）の合計が、色付け 5 次元の合計を必ず上回るようにする。
    //   ここが逆転していると「抑揚が大きい女性」が全部「おばあちゃん声」になる、
    //   といった破綻が起きる（実際に合成音声テストで発生した）。
    constexpr float kdF0 = 5.0f;
    constexpr float kdFs = 4.0f;
    constexpr float kdHnr = 1.0f;
    constexpr float kdBright = 0.8f;
    constexpr float kdVar = 0.8f;
    constexpr float kdMis = 0.9f;

    /** ★ジッターは判定に使わない（重み 0）。
        真のジッターは「周期ごと」のゆらぎだが、この分析器は 20 Hz のフレーム単位でしか
        f0 を出していないので、そこから取り出せるのは抑揚との区別がつかない量でしかない。
        実測でも多くの声で 1.0 に飽和し、判定に寄与できていなかった。
        値そのものは情報表示のために残してある。周期単位の検出を実装したら復活させる。 */
    constexpr float kdJit = 0.0f;

    using Tone = VoiceAnalyser::Tone;

    const Profile kProfiles[] =
    {
        //                        label                    tone            f0    fs   hnr  brgt  var   jit   mis
        { "野太い声",              Tone::masculine,  95.0f, 0.92f, 0.80f, 0.25f, 0.30f, 0.30f, 0.15f },
        { "男性的な低い声",        Tone::masculine, 112.0f, 0.97f, 0.85f, 0.35f, 0.40f, 0.20f, 0.15f },
        { "渋い声",                Tone::masculine, 118.0f, 0.95f, 0.58f, 0.30f, 0.25f, 0.38f, 0.15f },
        { "セクシー男声",          Tone::masculine, 108.0f, 0.96f, 0.50f, 0.30f, 0.25f, 0.30f, 0.15f },
        { "ダンディ声",            Tone::masculine, 128.0f, 0.96f, 0.80f, 0.40f, 0.30f, 0.20f, 0.15f },
        { "クールな男声",          Tone::masculine, 132.0f, 1.00f, 0.86f, 0.45f, 0.18f, 0.15f, 0.15f },
        { "お兄さん男声",          Tone::masculine, 145.0f, 1.00f, 0.85f, 0.50f, 0.45f, 0.20f, 0.15f },
        { "甘い男声",              Tone::masculine, 152.0f, 1.05f, 0.78f, 0.45f, 0.35f, 0.25f, 0.15f },
        { "爽やか男声",            Tone::masculine, 158.0f, 1.03f, 0.90f, 0.62f, 0.50f, 0.15f, 0.15f },
        { "男性的な高い声",        Tone::masculine, 168.0f, 1.02f, 0.85f, 0.55f, 0.45f, 0.20f, 0.15f },
        { "元気な男声",            Tone::masculine, 172.0f, 1.02f, 0.85f, 0.62f, 0.78f, 0.20f, 0.15f },
        { "カワイイ系な男声",      Tone::masculine, 192.0f, 1.12f, 0.85f, 0.66f, 0.62f, 0.20f, 0.20f },
        { "少年声",                Tone::masculine, 212.0f, 1.20f, 0.88f, 0.60f, 0.55f, 0.20f, 0.15f },
        { "おじいちゃん声",        Tone::masculine, 130.0f, 0.93f, 0.48f, 0.30f, 0.30f, 0.82f, 0.20f },

        { "女性的な低い声",        Tone::feminine,  176.0f, 1.10f, 0.80f, 0.45f, 0.40f, 0.20f, 0.15f },
        { "セクシー女声",          Tone::feminine,  186.0f, 1.12f, 0.48f, 0.40f, 0.30f, 0.30f, 0.15f },
        { "ハスキーな女声",        Tone::feminine,  196.0f, 1.15f, 0.42f, 0.45f, 0.35f, 0.42f, 0.15f },
        { "クールな女声",          Tone::feminine,  198.0f, 1.15f, 0.88f, 0.45f, 0.18f, 0.15f, 0.15f },
        { "お姉さん女声",          Tone::feminine,  204.0f, 1.16f, 0.85f, 0.45f, 0.35f, 0.20f, 0.15f },
        { "ナチュラルな女声",      Tone::feminine,  212.0f, 1.18f, 0.85f, 0.50f, 0.45f, 0.20f, 0.15f },
        { "お嬢様声",              Tone::feminine,  226.0f, 1.20f, 0.90f, 0.50f, 0.32f, 0.15f, 0.15f },
        { "ギャル女声",            Tone::feminine,  232.0f, 1.20f, 0.68f, 0.82f, 0.80f, 0.25f, 0.15f },
        { "元気な女声",            Tone::feminine,  242.0f, 1.22f, 0.85f, 0.72f, 0.80f, 0.20f, 0.15f },
        { "女性的な高い声",        Tone::feminine,  248.0f, 1.22f, 0.85f, 0.60f, 0.50f, 0.20f, 0.15f },
        { "アイドル声",            Tone::feminine,  256.0f, 1.25f, 0.88f, 0.72f, 0.75f, 0.20f, 0.15f },
        { "カワイイ系な女声",      Tone::feminine,  262.0f, 1.28f, 0.88f, 0.70f, 0.60f, 0.20f, 0.15f },
        { "甘えた声",              Tone::feminine,  272.0f, 1.30f, 0.74f, 0.60f, 0.55f, 0.30f, 0.20f },
        { "少女声",                Tone::feminine,  292.0f, 1.35f, 0.88f, 0.66f, 0.60f, 0.20f, 0.15f },
        { "おばあちゃん声",        Tone::feminine,  178.0f, 1.08f, 0.44f, 0.35f, 0.30f, 0.82f, 0.20f },

        { "中性声",                Tone::neutral,   186.0f, 1.08f, 0.80f, 0.50f, 0.40f, 0.20f, 0.15f },
        { "ダウナーな声",          Tone::neutral,   150.0f, 1.05f, 0.62f, 0.28f, 0.08f, 0.25f, 0.15f },
        { "落ち着いた声",          Tone::neutral,   166.0f, 1.05f, 0.86f, 0.40f, 0.20f, 0.15f, 0.15f },
    };

    /** ★加工感はプロファイル表に入れない。
        「f0 と声道長の組み合わせが人間離れしている」という別種の判定なので、
        同じ土俵で点数を競わせると壊れる（幅を広げると全部これになった）。
        通常の判定を出したあとに、この閾値を超えていたら差し替える。 */
    constexpr float kProcessedOverride = 0.90f;

    inline float bell (float x, float centre, float width) noexcept
    {
        const float t = (x - centre) / width;
        return std::exp (-t * t);
    }

    float scoreFor (const Profile& p, const Dims& d) noexcept
    {
        return kdF0     * bell (d.f0,           p.f0,     p.f0W > 0.0f ? p.f0W : kwF0)
             + kdFs     * bell (d.formantScale, p.fs,     p.fsW > 0.0f ? p.fsW : kwFs)
             + kdHnr    * bell (d.hnr,          p.hnr,    kwOther)
             + kdBright * bell (d.brightness,   p.bright, kwOther)
             + kdVar    * bell (d.variation,    p.var,    kwOther)
             + kdJit    * bell (d.jitter,       p.jit,    kwOther)
             + kdMis    * bell (d.mismatch,     p.mis,    kwMismatch);
    }

    //==========================================================================
    /** Levinson-Durbin。r は自己相関 0..order。失敗したら false。 */
    bool levinson (const float* r, int order, float* a) noexcept
    {
        if (r[0] <= 0.0f)
            return false;

        std::vector<float> tmp ((size_t) order + 1, 0.0f);
        float err = r[0];

        a[0] = 1.0f;

        for (int i = 1; i <= order; ++i)
            a[i] = 0.0f;

        for (int i = 1; i <= order; ++i)
        {
            float acc = r[i];

            for (int j = 1; j < i; ++j)
                acc -= a[j] * r[i - j];

            if (std::abs (err) < 1.0e-12f)
                return false;

            const float k = acc / err;

            for (int j = 0; j <= order; ++j)
                tmp[(size_t) j] = a[j];

            for (int j = 1; j < i; ++j)
                a[j] = tmp[(size_t) j] - k * tmp[(size_t) (i - j)];

            a[i] = k;
            err *= (1.0f - k * k);

            if (err <= 0.0f)
                return false;
        }

        // ここまでの a は「予測係数」。以降は 1 - Σ a_i z^-i の形で使う。
        return true;
    }
}

//==============================================================================
VoiceAnalyser::VoiceAnalyser (const RawRing& sourceToUse)
    : source (sourceToUse)
{
    window.resize ((size_t) kWindowSamples48, 0.0f);
    decimated.resize ((size_t) kWindowSamples48, 0.0f);
    lpcWork.resize ((size_t) kLpcOrder + 1, 0.0f);
    f0History.reserve (64);
}

void VoiceAnalyser::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    decimation   = juce::jmax (1, juce::roundToInt (sampleRate / 12000.0));
    analysisRate = sampleRate / (double) decimation;

    // デシメーション前のアンチエイリアス（解析レートの 45%）。
    {
        const double fc = analysisRate * 0.45;
        const double w0 = 2.0 * juce::MathConstants<double>::pi * fc / sampleRate;
        const double cosW = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * 0.7071);
        const double a0 = 1.0 + alpha;

        lpB0 = (float) (((1.0 - cosW) * 0.5) / a0);
        lpB1 = (float) ((1.0 - cosW) / a0);
        lpB2 = lpB0;
        lpA1 = (float) ((-2.0 * cosW) / a0);
        lpA2 = (float) ((1.0 - alpha) / a0);
    }

    // 明るさ用。2 kHz 以上のエネルギー比を見る。
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * 2000.0 / sampleRate;
        const double cosW = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * 0.7071);
        const double a0 = 1.0 + alpha;

        hpB0 = (float) (((1.0 + cosW) * 0.5) / a0);
        hpB1 = (float) (-(1.0 + cosW) / a0);
        hpB2 = hpB0;
        hpA1 = (float) ((-2.0 * cosW) / a0);
        hpA2 = (float) ((1.0 - alpha) / a0);
    }

    reset();
}

void VoiceAnalyser::reset()
{
    f0History.clear();
    sumFormantScale = sumHnr = sumBrightness = 0.0f;
    voicedFrames = 0;
    accumulatedSeconds = 0.0;

    speaking = false;
    silenceSeconds = 0.0;
    levelDb = -100.0f;
    noiseFloorDb = -70.0f;

    hpX1 = hpX2 = hpY1 = hpY2 = 0.0f;
    lpX1 = lpX2 = lpY1 = lpY2 = 0.0f;

    result = {};
}

//==============================================================================
bool VoiceAnalyser::analyseFrame (Features& out)
{
    source.readLatest (window.data(), kWindowSamples48);

    // --- レベルと明るさ
    double sumSq = 0.0, sumHigh = 0.0;

    hpX1 = hpX2 = hpY1 = hpY2 = 0.0f;

    for (int i = 0; i < kWindowSamples48; ++i)
    {
        const float x = window[(size_t) i];
        sumSq += (double) x * x;

        const float y = hpB0 * x + hpB1 * hpX1 + hpB2 * hpX2 - hpA1 * hpY1 - hpA2 * hpY2;
        hpX2 = hpX1; hpX1 = x;
        hpY2 = hpY1; hpY1 = y;

        sumHigh += (double) y * y;
    }

    const float rms = (float) std::sqrt (sumSq / (double) kWindowSamples48);
    levelDb = rms > 1.0e-7f ? 20.0f * std::log10 (rms) : -100.0f;

    out.brightness = sumSq > 1.0e-12 ? juce::jlimit (0.0f, 1.0f,
                                                     (float) std::sqrt (sumHigh / sumSq) * 1.6f)
                                     : 0.0f;

    // --- ノイズフロアの追従。上がるのは遅く、下がるのは速く。
    if (levelDb < noiseFloorDb)
        noiseFloorDb += 0.35f * (levelDb - noiseFloorDb);
    else
        noiseFloorDb += 0.004f * (levelDb - noiseFloorDb);

    noiseFloorDb = juce::jlimit (-90.0f, -20.0f, noiseFloorDb);

    const bool loudEnough = levelDb > kAbsoluteSilenceDb
                             && levelDb > noiseFloorDb + kSpeechMarginDb;

    if (! loudEnough)
    {
        out.voiced = false;
        return false;
    }

    // --- 12 kHz へ落とす
    lpX1 = lpX2 = lpY1 = lpY2 = 0.0f;
    int n = 0;

    for (int i = 0; i < kWindowSamples48; ++i)
    {
        const float x = window[(size_t) i];
        const float y = lpB0 * x + lpB1 * lpX1 + lpB2 * lpX2 - lpA1 * lpY1 - lpA2 * lpY2;
        lpX2 = lpX1; lpX1 = x;
        lpY2 = lpY1; lpY1 = y;

        if ((i % decimation) == 0)
            decimated[(size_t) n++] = y;
    }

    if (n < 256)
    {
        out.voiced = false;
        return false;
    }

    // --- f0 と HNR（正規化自己相関のピーク）
    const int minLag = juce::jmax (2, (int) (analysisRate / kMaxF0));
    const int maxLag = juce::jmin (n / 2 - 1, (int) (analysisRate / kMinF0));

    if (maxLag <= minLag)
    {
        out.voiced = false;
        return false;
    }

    // ★素の自己相関のピークを採ってはいけない。正規化しても短いラグほど
    //   足し合わせる項数が多くて有利になり、高い方へ引きずられる。実測で
    //   115 Hz のかすれ声が 480 Hz と判定された。YIN の CMNDF はこの偏りを
    //   累積平均で正規化して消すために作られている。
    const int compareWindow = juce::jmin (n - maxLag - 1, n / 2);

    if (compareWindow < 128)
    {
        out.voiced = false;
        return false;
    }

    static std::vector<double> diff, cmndf;
    diff .assign ((size_t) maxLag + 1, 0.0);
    cmndf.assign ((size_t) maxLag + 1, 1.0);

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        double acc = 0.0;

        for (int i = 0; i < compareWindow; ++i)
        {
            const double delta = (double) decimated[(size_t) i]
                                  - (double) decimated[(size_t) (i + tau)];
            acc += delta * delta;
        }

        diff[(size_t) tau] = acc;
    }

    double running = 0.0;

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        running += diff[(size_t) tau];
        cmndf[(size_t) tau] = running > 1.0e-12 ? diff[(size_t) tau] * (double) tau / running : 1.0;
    }

    // 閾値を最初に下回った谷を採る。全体の最小値を採るとオクターブ下に飛ぶ。
    constexpr double kVoicedThreshold = 0.15;

    int bestLag = 0;

    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (cmndf[(size_t) tau] >= kVoicedThreshold)
            continue;

        while (tau + 1 <= maxLag && cmndf[(size_t) (tau + 1)] < cmndf[(size_t) tau])
            ++tau;

        bestLag = tau;
        break;
    }

    if (bestLag == 0)
    {
        double lowest = 1.0e9;

        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            if (cmndf[(size_t) tau] < lowest)
            {
                lowest = cmndf[(size_t) tau];
                bestLag = tau;
            }
        }
    }

    if (bestLag <= 0 || cmndf[(size_t) bestLag] > (1.0 - kMinVoicedHnr))
    {
        out.voiced = false;   // 周期性が無い＝子音・暗騒音・エアコン
        return false;
    }

    // 放物線補間でサンプル間の精度を出す（12 kHz だと 1 ラグが数 Hz 効く）。
    double refined = bestLag;

    if (bestLag > 1 && bestLag < maxLag)
    {
        const double a = cmndf[(size_t) (bestLag - 1)];
        const double b = cmndf[(size_t) bestLag];
        const double c = cmndf[(size_t) (bestLag + 1)];
        const double den = 2.0 * (2.0 * b - a - c);

        if (std::abs (den) > 1.0e-12)
            refined = bestLag + (c - a) / den;
    }

    out.f0  = (float) (analysisRate / juce::jmax (1.0, refined));
    out.hnr = juce::jlimit (0.0f, 1.0f, (float) (1.0 - cmndf[(size_t) bestLag]));

    // --- LPC でフォルマント（包絡のピーク 2 本）
    {
        // プリエンファシスとハミング窓
        static std::vector<float> pre;
        pre.resize ((size_t) n);

        float prev = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float x = decimated[(size_t) i];
            pre[(size_t) i] = x - 0.97f * prev;
            prev = x;
        }

        for (int i = 0; i < n; ++i)
            pre[(size_t) i] *= 0.54f - 0.46f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                          * (float) i / (float) (n - 1));

        std::vector<float> r ((size_t) kLpcOrder + 1, 0.0f);

        for (int k = 0; k <= kLpcOrder; ++k)
        {
            double acc = 0.0;

            for (int i = k; i < n; ++i)
                acc += (double) pre[(size_t) i] * pre[(size_t) (i - k)];

            r[(size_t) k] = (float) acc;
        }

        std::vector<float> a ((size_t) kLpcOrder + 1, 0.0f);

        if (levinson (r.data(), kLpcOrder, a.data()))
        {
            // 包絡を 0〜3.5 kHz で評価して山を 2 つ拾う。
            constexpr int kPoints = 220;
            const float maxHz = 3500.0f;

            float mags[kPoints];

            for (int p = 0; p < kPoints; ++p)
            {
                const float hz = maxHz * (float) p / (float) (kPoints - 1);
                const float w = 2.0f * juce::MathConstants<float>::pi * hz / (float) analysisRate;

                float re = 1.0f, im = 0.0f;

                for (int k = 1; k <= kLpcOrder; ++k)
                {
                    re -= a[(size_t) k] * std::cos (w * (float) k);
                    im += a[(size_t) k] * std::sin (w * (float) k);
                }

                const float den = re * re + im * im;
                mags[p] = den > 1.0e-12f ? 1.0f / den : 0.0f;
            }

            float f1 = 0.0f, f2 = 0.0f;

            for (int p = 1; p < kPoints - 1; ++p)
            {
                if (mags[p] <= mags[p - 1] || mags[p] <= mags[p + 1])
                    continue;

                const float hz = maxHz * (float) p / (float) (kPoints - 1);

                if (hz < 200.0f)
                    continue;

                if (f1 <= 0.0f)          f1 = hz;
                else if (f2 <= 0.0f)   { f2 = hz; break; }
            }

            if (f1 > 0.0f && f2 > 0.0f)
            {
                // 平均男性を 1.0 とする指標。F1=500 / F2=1500 を基準にした。
                out.formantScale = juce::jlimit (0.75f, 1.60f,
                                                 0.5f * (f1 / 500.0f + f2 / 1500.0f));
            }
            else
            {
                out.formantScale = expectedFormantScale (out.f0);
            }
        }
        else
        {
            out.formantScale = expectedFormantScale (out.f0);
        }
    }

    out.voiced = true;
    return true;
}

//==============================================================================
bool VoiceAnalyser::tick (double updateIntervalSeconds)
{
    const double dt = kTickIntervalMs / 1000.0;

    Features f;
    const bool voiced = analyseFrame (f);

    if (voiced)
    {
        if (! speaking)
        {
            // 発話の開始。前回の区間を引きずらないように積算を捨てる。
            f0History.clear();
            sumFormantScale = sumHnr = sumBrightness = 0.0f;
            voicedFrames = 0;
            accumulatedSeconds = 0.0;
        }

        speaking = true;
        silenceSeconds = 0.0;

        f0History.push_back (f.f0);
        sumFormantScale += f.formantScale;
        sumHnr += f.hnr;
        sumBrightness += f.brightness;
        ++voicedFrames;
    }
    else
    {
        silenceSeconds += dt;

        if (silenceSeconds >= kSilenceHoldSeconds)
            speaking = false;
    }

    if (! speaking)
        return false;   // 無音中は最後の判定を出したまま何もしない

    accumulatedSeconds += dt;

    if (accumulatedSeconds < updateIntervalSeconds)
        return false;

    accumulatedSeconds = 0.0;

    // 区間の大半が有声でなければ判定しない。数フレームだけ周期性を持った
    // 雑音で「声の特徴」が出てしまうのを防ぐ（実際に暗騒音で誤判定が出た）。
    const int expectedFrames = juce::jmax (1, (int) (updateIntervalSeconds * 1000.0
                                                      / (double) kTickIntervalMs));
    const int required = juce::jmax (4, juce::roundToInt (expectedFrames * kMinVoicedRatio));

    if (voicedFrames < required)
    {
        f0History.clear();
        sumFormantScale = sumHnr = sumBrightness = 0.0f;
        voicedFrames = 0;
        return false;
    }

    classify();

    f0History.clear();
    sumFormantScale = sumHnr = sumBrightness = 0.0f;
    voicedFrames = 0;

    return true;
}

//==============================================================================
void VoiceAnalyser::classify()
{
    const int count = (int) f0History.size();

    if (count <= 0)
        return;

    // f0 は外れ値に強い中央値で代表させる（オクターブ誤検出の保険）。
    std::vector<float> sorted = f0History;
    std::sort (sorted.begin(), sorted.end());

    Dims d;
    d.f0 = sorted[(size_t) (count / 2)];
    d.formantScale = sumFormantScale / (float) count;
    d.hnr = sumHnr / (float) count;
    d.brightness = sumBrightness / (float) count;

    // 抑揚: 半音での標準偏差。4 半音で頭打ちにする。
    {
        double mean = 0.0;

        for (const float v : f0History)
            mean += 12.0 * std::log2 (juce::jmax (1.0f, v) / 100.0f);

        mean /= (double) count;

        double var = 0.0;

        for (const float v : f0History)
        {
            const double s = 12.0 * std::log2 (juce::jmax (1.0f, v) / 100.0f) - mean;
            var += s * s;
        }

        d.variation = juce::jlimit (0.0f, 1.0f,
                                    (float) std::sqrt (var / (double) count) / 4.0f);
    }

    // 揺れ: 5 点移動平均からの残差。
    //
    // ★隣接フレームの差をそのまま使ってはいけない。解析は 20 Hz なので、
    //   5 Hz 前後のビブラート（＝抑揚）がフレーム間差にそのまま乗ってしまい、
    //   「抑揚が大きい声」を全部「年配の声」と誤判定する。実際にそうなった。
    //   5 点（250 ms）はビブラート 1 周期をほぼ打ち消すので、
    //   残るのは周期性のない細かい揺れだけになる。
    {
        double acc = 0.0;
        int jitterPairs = 0;

        for (int i = 2; i + 2 < (int) f0History.size(); ++i)
        {
            const float centre = f0History[(size_t) i];

            if (centre <= 1.0f)
                continue;

            const float smooth = (f0History[(size_t) (i - 2)] + f0History[(size_t) (i - 1)]
                                   + centre
                                   + f0History[(size_t) (i + 1)] + f0History[(size_t) (i + 2)]) / 5.0f;

            acc += std::abs (centre - smooth) / centre;
            ++jitterPairs;
        }

        d.jitter = jitterPairs > 0 ? juce::jlimit (0.0f, 1.0f, (float) (acc / jitterPairs) / 0.030f)
                             : 0.0f;
    }

    d.mismatch = juce::jlimit (0.0f, 1.0f,
                               std::abs (d.formantScale - expectedFormantScale (d.f0)) / 0.25f);

    // --- 総当たりで一番近いプロファイルを選ぶ
    const Profile* best = nullptr;
    float bestScore = -1.0f, secondScore = -1.0f;

    for (const auto& p : kProfiles)
    {
        const float s = scoreFor (p, d);

        if (s > bestScore)
        {
            secondScore = bestScore;
            bestScore = s;
            best = &p;
        }
        else if (s > secondScore)
        {
            secondScore = s;
        }
    }

    if (best == nullptr)
        return;

    measured = { d.f0, d.formantScale, d.hnr, d.brightness, d.variation, d.jitter, d.mismatch };

    if (d.mismatch >= kProcessedOverride)
    {
        // 声の高さと声道長の組み合わせが人間の範囲から外れている。
        // どのカテゴリに寄せても嘘になるので、そう言う。
        result.label = jp ("加工感の強い声");
        result.tone = Tone::neutral;
        result.confidence = juce::jlimit (0.0f, 1.0f, (d.mismatch - kProcessedOverride) * 6.0f + 0.4f);
        result.valid = true;
        return;
    }

    result.label = jp (best->label);
    result.tone = best->tone;
    result.valid = true;

    // 1 位と 2 位が僅差なら確信度は低い。表示の濃さに使う。
    const float gap = bestScore - juce::jmax (0.0f, secondScore);
    result.confidence = juce::jlimit (0.0f, 1.0f, gap / 1.2f);
}

} // namespace kvc
