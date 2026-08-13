// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "TestSupport.h"

#include "core/PitchFormantShifter.h"

#include <cmath>
#include <string>

using namespace kvctest;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr int    kBlock = 128;

    /** 合成母音 /a/。F1 700 / F2 1200 / F3 2600 は日本語の「あ」に近い並び。 */
    const std::vector<Formant> kVowelA { { 700.0, 90.0 }, { 1200.0, 110.0 }, { 2600.0, 160.0 } };

    /** 実運用と同じくブロック単位で流し込む。 */
    std::vector<float> runShifter (const std::vector<float>& input,
                                   float pitchSemitones, float formantSemitones,
                                   float wetMix = 1.0f, int blockSize = kBlock)
    {
        kvc::PitchFormantShifter shifter;

        shifter.setPitchSemitones (pitchSemitones);
        shifter.setFormantSemitones (formantSemitones);
        shifter.setWetMix (wetMix);
        shifter.prepare (kRate, blockSize);

        std::vector<float> output (input.size(), 0.0f);

        for (size_t offset = 0; offset < input.size(); offset += static_cast<size_t> (blockSize))
        {
            const int n = static_cast<int> (juce::jmin (static_cast<size_t> (blockSize),
                                                        input.size() - offset));
            shifter.process (input.data() + offset, output.data() + offset, n);
        }

        return output;
    }

    /** 立ち上がり（先読み充填 + ピッチ追従 + 半音のスルーレート）を捨てた後半だけを測る。 */
    struct Tail
    {
        const float* data;
        int numSamples;
    };

    Tail tailOf (const std::vector<float>& v, double skipSeconds = 0.6)
    {
        const int skip = static_cast<int> (skipSeconds * kRate);
        const int n = static_cast<int> (v.size()) - skip;

        return { v.data() + skip, juce::jmax (0, n) };
    }

    std::string hz (double value)
    {
        char buffer[64];
        std::snprintf (buffer, sizeof (buffer), "%.1f Hz", value);
        return buffer;
    }

    /** ピッチを semitones だけ動かしたときの出力 F0 を測って返す。 */
    double measureShiftedF0 (double inputF0, float pitchSemitones, float formantSemitones = 0.0f)
    {
        const auto input = makeVowel (kRate, inputF0, 3.0, kVowelA);
        const auto output = runShifter (input, pitchSemitones, formantSemitones);
        const auto tail = tailOf (output);

        KVC_CHECK (allFinite (tail.data, tail.numSamples));
        KVC_CHECK (rms (tail.data, tail.numSamples) > 0.01);

        return estimateF0 (tail.data, tail.numSamples, kRate);
    }
}

//==============================================================================
// ピッチ
//==============================================================================

KVC_TEST (PitchShift, unity_keeps_pitch_and_formants)
{
    const auto input = makeVowel (kRate, 140.0, 3.0, kVowelA);
    const auto output = runShifter (input, 0.0f, 0.0f);

    const auto inTail = tailOf (input);
    const auto outTail = tailOf (output);

    const double inF0  = estimateF0 (inTail.data, inTail.numSamples, kRate);
    const double outF0 = estimateF0 (outTail.data, outTail.numSamples, kRate);

    const double inEnv  = envelopePeakHz (inTail.data, inTail.numSamples, kRate);
    const double outEnv = envelopePeakHz (outTail.data, outTail.numSamples, kRate);

    note ("in  F0 " + hz (inF0)  + ", envelope peak " + hz (inEnv));
    note ("out F0 " + hz (outF0) + ", envelope peak " + hz (outEnv));

    KVC_CHECK_NEAR (outF0, inF0, 0.03);
    KVC_CHECK_NEAR (outEnv, inEnv, 0.25);
}

KVC_TEST (PitchShift, up_one_octave_male)
{
    const double f0 = measureShiftedF0 (120.0, 12.0f);
    note ("120 Hz +12 st -> " + hz (f0) + " (expect 240 Hz)");
    KVC_CHECK_NEAR (f0, 240.0, 0.06);
}

KVC_TEST (PitchShift, is_phase_invariant)
{
    // グレインの中心が声門パルスに載っていないと、隣のパルスが窓の両端で半分ずつ
    // 生き残り、ピッチを下げても元の高さが残る。しかもそれは入力の位相次第で
    // 起きたり起きなかったりする。入力を 1 周期ぶん舐めて、どの位相でも
    // 同じ結果になることを確かめる。
    const auto full = makeVowel (kRate, 240.0, 3.2, kVowelA);
    const int length = static_cast<int> (3.0 * kRate);

    for (int shift = 0; shift < 200; shift += 25)
    {
        const std::vector<float> input (full.begin() + shift, full.begin() + shift + length);
        const auto output = runShifter (input, -12.0f, 0.0f);
        const auto t = tailOf (output);

        const double f0 = estimateF0 (t.data, t.numSamples, kRate);

        note ("input phase +" + std::to_string (shift) + " -> " + hz (f0));

        KVC_CHECK_NEAR (f0, 120.0, 0.06);
    }
}

KVC_TEST (Quality, matches_an_ideal_synthetic_voice)
{
    note ("lookahead = " + std::to_string (kvc::PitchFormantShifter::kLookaheadSamples) + " samples");

    struct Case { double base; float pitch; float formant; };

    for (Case c : { Case { 120.0, 12.0f, 0.0f },
                    Case { 120.0, -5.0f, 0.0f },
                    Case { 130.0,  4.0f, 3.0f },
                    Case { 150.0,  0.0f, 0.0f },
                    Case { 200.0, -12.0f, 0.0f },
                    Case { 220.0,  7.0f, 5.0f },
                    Case {  90.0,  7.0f, 0.0f } })
    {
        const auto input = makeVowel (kRate, c.base, 3.0, kVowelA);
        const auto output = runShifter (input, c.pitch, c.formant);
        const auto t = tailOf (output);

        const double target = c.base * std::pow (2.0, c.pitch / 12.0);

        // 理想: 同じフォルマントで、目標の高さのまま合成した音。
        // フォルマントを動かした場合は共振周波数も同じだけ動かして基準を作る。
        const double formantRatio = std::pow (2.0, c.formant / 12.0);
        std::vector<Formant> shifted;

        for (auto f : kVowelA)
            shifted.push_back ({ f.frequency * formantRatio, f.bandwidth * formantRatio });

        const auto ideal = makeVowel (kRate, target, 3.0, shifted);
        const auto it = tailOf (ideal);

        const double f0 = estimateF0 (t.data, t.numSamples, kRate);
        const double distance = logSpectralDistanceDb (t.data, t.numSamples, it.data, it.numSamples, kRate);
        const double am = amplitudeModulationDepth (t.data, t.numSamples, kRate);

        char line[256];
        std::snprintf (line, sizeof (line),
                       "%5.0f Hz  p%+5.1f f%+5.1f -> F0 %6.1f (want %6.1f, err %5.2f%%)  spec %5.2f dB  AM %5.3f",
                       c.base, c.pitch, c.formant, f0, target,
                       100.0 * (f0 - target) / target, distance, am);
        note (line);

        // 数値そのものは「測定記録」として CI のログに残すのが主目的だが、
        // 大きく崩れたら落ちるように緩い門も置いておく。
        KVC_CHECK_NEAR (f0, target, 0.03);
        KVC_CHECK (distance < 4.0);
        KVC_CHECK (am < 0.8);
    }
}

KVC_TEST (PitchShift, down_one_octave_female)
{
    const double f0 = measureShiftedF0 (240.0, -12.0f);
    note ("240 Hz -12 st -> " + hz (f0) + " (expect 120 Hz)");
    KVC_CHECK_NEAR (f0, 120.0, 0.06);
}

KVC_TEST (PitchShift, up_seven_semitones)
{
    const double expected = 130.0 * std::pow (2.0, 7.0 / 12.0);
    const double f0 = measureShiftedF0 (130.0, 7.0f);
    note ("130 Hz +7 st -> " + hz (f0) + " (expect " + hz (expected) + ")");
    KVC_CHECK_NEAR (f0, expected, 0.06);
}

KVC_TEST (PitchShift, down_five_semitones)
{
    const double expected = 200.0 * std::pow (2.0, -5.0 / 12.0);
    const double f0 = measureShiftedF0 (200.0, -5.0f);
    note ("200 Hz -5 st -> " + hz (f0) + " (expect " + hz (expected) + ")");
    KVC_CHECK_NEAR (f0, expected, 0.06);
}

KVC_TEST (PitchShift, monotonic_across_the_whole_range)
{
    // 「つまみを上げたのに下がる」は，個々の点が合っていても起こりうる。
    // -12..+12 を 3 半音刻みで舐めて，単調増加であることを直接確かめる。
    double previous = 0.0;

    for (int semitones = -12; semitones <= 12; semitones += 3)
    {
        const double f0 = measureShiftedF0 (150.0, static_cast<float> (semitones));
        const double expected = 150.0 * std::pow (2.0, semitones / 12.0);

        note (std::to_string (semitones) + " st -> " + hz (f0) + " (expect " + hz (expected) + ")");

        KVC_CHECK_NEAR (f0, expected, 0.07);
        KVC_CHECK (f0 > previous);

        previous = f0;
    }
}

KVC_TEST (PitchShift, keeps_formants_when_pitch_moves)
{
    // PSOLA の存在意義そのもの: ピッチを 1 オクターブ上げても包絡は動かない。
    // （ストリーム全体をリサンプルすると，ここが 2 倍に動いて「早送り声」になる）
    const auto input = makeVowel (kRate, 130.0, 3.0, kVowelA);

    const auto flat = runShifter (input, 0.0f, 0.0f);
    const auto up   = runShifter (input, 12.0f, 0.0f);

    const auto flatTail = tailOf (flat);
    const auto upTail   = tailOf (up);

    const double flatEnv = envelopePeakHz (flatTail.data, flatTail.numSamples, kRate);
    const double upEnv   = envelopePeakHz (upTail.data, upTail.numSamples, kRate);

    note ("envelope peak: pitch 0 st " + hz (flatEnv) + " -> pitch +12 st " + hz (upEnv));

    KVC_CHECK_NEAR (upEnv, flatEnv, 0.30);
}

//==============================================================================
// フォルマント
//==============================================================================

KVC_TEST (FormantShift, up_moves_envelope_but_not_pitch)
{
    const auto input = makeVowel (kRate, 150.0, 3.0, kVowelA);

    const auto flat = runShifter (input, 0.0f, 0.0f);
    const auto up   = runShifter (input, 0.0f, 12.0f);

    const auto flatTail = tailOf (flat);
    const auto upTail   = tailOf (up);

    const double flatEnv = envelopePeakHz (flatTail.data, flatTail.numSamples, kRate);
    const double upEnv   = envelopePeakHz (upTail.data, upTail.numSamples, kRate);

    const double flatF0 = estimateF0 (flatTail.data, flatTail.numSamples, kRate);
    const double upF0   = estimateF0 (upTail.data, upTail.numSamples, kRate);

    note ("envelope peak " + hz (flatEnv) + " -> " + hz (upEnv) + " (expect roughly doubled)");
    note ("F0 " + hz (flatF0) + " -> " + hz (upF0) + " (expect unchanged)");

    KVC_CHECK (upEnv > flatEnv * 1.5);
    KVC_CHECK_NEAR (upF0, flatF0, 0.05);
}

KVC_TEST (FormantShift, down_moves_envelope_but_not_pitch)
{
    const auto input = makeVowel (kRate, 220.0, 3.0, kVowelA);

    const auto flat = runShifter (input, 0.0f, 0.0f);
    const auto down = runShifter (input, 0.0f, -12.0f);

    const auto flatTail = tailOf (flat);
    const auto downTail = tailOf (down);

    const double flatEnv = envelopePeakHz (flatTail.data, flatTail.numSamples, kRate);
    const double downEnv = envelopePeakHz (downTail.data, downTail.numSamples, kRate);

    const double flatF0 = estimateF0 (flatTail.data, flatTail.numSamples, kRate);
    const double downF0 = estimateF0 (downTail.data, downTail.numSamples, kRate);

    note ("envelope peak " + hz (flatEnv) + " -> " + hz (downEnv) + " (expect roughly halved)");
    note ("F0 " + hz (flatF0) + " -> " + hz (downF0) + " (expect unchanged)");

    KVC_CHECK (downEnv < flatEnv * 0.75);
    KVC_CHECK_NEAR (downF0, flatF0, 0.05);
}

KVC_TEST (FormantShift, is_independent_of_pitch_shift)
{
    // 2 つのつまみが直交していること。ピッチ +12 のまま，フォルマントだけを動かす。
    const auto input = makeVowel (kRate, 130.0, 3.0, kVowelA);

    const auto pitchOnly = runShifter (input, 12.0f, 0.0f);
    const auto both      = runShifter (input, 12.0f, 7.0f);

    const auto a = tailOf (pitchOnly);
    const auto b = tailOf (both);

    const double f0A = estimateF0 (a.data, a.numSamples, kRate);
    const double f0B = estimateF0 (b.data, b.numSamples, kRate);

    const double envA = envelopePeakHz (a.data, a.numSamples, kRate);
    const double envB = envelopePeakHz (b.data, b.numSamples, kRate);

    note ("F0 " + hz (f0A) + " -> " + hz (f0B) + " (expect unchanged)");
    note ("envelope peak " + hz (envA) + " -> " + hz (envB) + " (expect higher)");

    KVC_CHECK_NEAR (f0B, f0A, 0.06);
    KVC_CHECK (envB > envA * 1.2);
}

//==============================================================================
// 契約（遅延・バイパス・安定性）
//==============================================================================

KVC_TEST (Shifter, latency_is_constant_for_every_setting)
{
    kvc::PitchFormantShifter shifter;
    shifter.prepare (kRate, kBlock);

    const int expected = kvc::PitchFormantShifter::kLookaheadSamples;

    for (float pitch : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        for (float formant : { -12.0f, 0.0f, 12.0f })
        {
            shifter.setPitchSemitones (pitch);
            shifter.setFormantSemitones (formant);
            KVC_CHECK (shifter.getLatencySamples() == expected);
        }
    }

    for (float wet : { 0.0f, 0.5f, 1.0f })
    {
        shifter.setWetMix (wet);
        KVC_CHECK (shifter.getLatencySamples() == expected);
    }
}

KVC_TEST (Shifter, dry_path_is_an_exact_delay)
{
    // 「声を変える」を OFF にしたときは，先読みぶん遅れただけの原音でなければならない。
    // ここがずれると，切り替えのたびに遅延補正が破綻してプチッと鳴る。
    const auto input = makeVowel (kRate, 150.0, 1.0, kVowelA);
    const auto output = runShifter (input, 5.0f, 5.0f, 0.0f);

    const int latency = kvc::PitchFormantShifter::kLookaheadSamples;

    double worst = 0.0;

    for (int i = latency; i < static_cast<int> (input.size()); ++i)
        worst = juce::jmax (worst, std::abs (static_cast<double> (output[static_cast<size_t> (i)])
                                                 - static_cast<double> (input[static_cast<size_t> (i - latency)])));

    note ("worst sample error " + std::to_string (worst));

    KVC_CHECK (worst < 1.0e-6);
}

KVC_TEST (Shifter, survives_noise_and_silence_without_blowing_up)
{
    std::vector<float> input;

    const auto noise = makeNoise (static_cast<int> (kRate), 0.3f);
    input.insert (input.end(), noise.begin(), noise.end());

    input.insert (input.end(), static_cast<size_t> (kRate / 2), 0.0f);

    const auto vowel = makeVowel (kRate, 110.0, 1.0, kVowelA);
    input.insert (input.end(), vowel.begin(), vowel.end());

    for (float semitones : { -12.0f, 0.0f, 12.0f })
    {
        const auto output = runShifter (input, semitones, -semitones);

        KVC_CHECK (allFinite (output.data(), static_cast<int> (output.size())));
        KVC_CHECK (peak (output.data(), static_cast<int> (output.size())) < 4.0);
    }
}

KVC_TEST (Shifter, block_size_does_not_change_the_result)
{
    // 実機のブロック長はデバイス任せ。ブロック長で音が変わるなら，
    // それはリングかエポックの帳簿が壊れているということ。
    const auto input = makeVowel (kRate, 160.0, 2.0, kVowelA);

    double reference = 0.0;

    for (int blockSize : { 32, 64, 128, 256, 480 })
    {
        const auto output = runShifter (input, 12.0f, 0.0f, 1.0f, blockSize);
        const auto tail = tailOf (output);

        const double f0 = estimateF0 (tail.data, tail.numSamples, kRate);

        note ("block " + std::to_string (blockSize) + " -> " + hz (f0));

        KVC_CHECK (allFinite (tail.data, tail.numSamples));
        KVC_CHECK_NEAR (f0, 320.0, 0.08);

        if (reference > 0.0)
            KVC_CHECK_NEAR (f0, reference, 0.05);

        reference = f0;
    }
}

KVC_TEST (Shifter, gain_stays_close_to_unity)
{
    // OLA の正規化が効いていること。効いていないと，つまみを動かすたびに音量が跳ねる。
    const auto input = makeVowel (kRate, 150.0, 2.0, kVowelA);
    const auto inTail = tailOf (input);
    const double inRms = rms (inTail.data, inTail.numSamples);

    for (int semitones : { -12, -6, 0, 6, 12 })
    {
        const auto output = runShifter (input, static_cast<float> (semitones), 0.0f);
        const auto outTail = tailOf (output);
        const double outRms = rms (outTail.data, outTail.numSamples);

        const double dB = 20.0 * std::log10 (juce::jmax (1.0e-9, outRms / inRms));

        note (std::to_string (semitones) + " st -> " + std::to_string (dB) + " dB");

        KVC_CHECK_IN_RANGE (dB, -8.0, 8.0);
    }
}
