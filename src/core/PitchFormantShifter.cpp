// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "core/PitchFormantShifter.h"

#include <cmath>
#include <cstdint>

namespace kvc
{

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    /** 相関探索の上限（48 kHz サンプル）。
        探索は「T0 の丸め誤差を直す」ためだけのもので、参照は毎回 lastMark に取り直すので
        誤差は 1 グレインぶんしか溜まらない。未来向きの探索はそのまま先読み予算を食うため、
        v1.0.0 の 96 から 32 へ詰めた。 */
    constexpr int kSearchCap = 32;
    constexpr int kSearchFloor = 8;

    /** 予算の安全マージン。T0 と F はグレイン間で動くので、
        「合成をやめた時点」と「次のグレイン」でパラメータが 1 段ずれても破綻しない余裕。 */
    constexpr int kBudgetMargin = 16;

    constexpr int kMinHalfOut = 8;
    constexpr int kMaxHalfOut = 1024;         ///< 片側の上限（リング 8192 に対する安全側）
    constexpr int kFineRefine = 3;            ///< 48 kHz での ±3 サンプル追い込み

    /** 1 エポックで解析マークを進められる周期数の上限。
        T0 の推定が一瞬壊れても、グレインが遠くへ飛ばないための蓋。 */
    constexpr int kMaxAdvance = 8;

    /** 負の絶対位置でも正しく回るリングインデックス。
        int64 の & はビット演算なので、2 の補数のまま剰余として正しく働く。 */
    inline int ringIndex (int64_t p, int mask) noexcept
    {
        return static_cast<int> (static_cast<uint64_t> (p) & static_cast<uint64_t> (mask));
    }

    /** 8 kHz リング用の床除算。p が負でも -1 側へ丸める（切り捨てだと 0 付近で 1 サンプルずれる）。 */
    inline int64_t floorDiv (int64_t p, int d) noexcept
    {
        return p >= 0 ? p / d : -(((-p) + d - 1) / d);
    }

    /** 4 点 3 次 Hermite（Catmull-Rom）。0.4*fs まで約 -60 dB。 */
    inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    /** xorshift32。オーディオスレッドから rand() は呼べないので自前で持つ。戻り値は -1..1。 */
    inline float nextJitter (uint32_t& state) noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;

        return static_cast<float> (static_cast<int32_t> (state)) * (1.0f / 2147483648.0f);
    }

}

//==============================================================================
PitchFormantShifter::PitchFormantShifter() = default;
PitchFormantShifter::~PitchFormantShifter() = default;

//==============================================================================
void PitchFormantShifter::prepare (double newSampleRate, int /*maxBlockSize*/)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    tracker.prepare (sampleRate);

    // 0: in48 / 1: outAccum / 2: dry（遅延したドライ経路）
    rings.setSize (3, kRingSize, false, true, false);

    // 0: 連続化したグレイン素材（最大 hInLeft + hInRight + 4）
    // 1: 窓テーブル（前半 kWindowTableSize = 立上り / 後半 = 立下り）
    grain.setSize (2, 4096, false, true, false);

    // 0: 8 kHz の参照セグメント / 1: 48 kHz の参照セグメント
    prevSeg.setSize (2, 512, false, true, false);

    wetMix.reset (sampleRate, 0.020);

    buildWindowTables();
    reset();
}

//==============================================================================
void PitchFormantShifter::buildWindowTables() noexcept
{
    float* const table = grain.getWritePointer (1);

    for (int i = 0; i < kWindowTableSize; ++i)
    {
        const float u = static_cast<float> (i) / static_cast<float> (kWindowTableSize);

        // 立上り: レイズドコサイン（ハン窓の前半）
        table[i] = 0.5f * (1.0f - std::cos (kPi * u));

        // 立下り: cos^2.5。大きなダウンシフトでゲート感を出さないための 2.5 乗。
        // pow を避けて c*c*sqrt(c) で厳密に同じ値を出す（c >= 0 が保証される区間）。
        const float c = std::cos (kPi * u * 0.5f);
        table[kWindowTableSize + i] = c * c * std::sqrt (c);
    }

    // OLA のゲインを解析的に出すための平均値。テーブルが変われば自動で追随する。
    double sumRise = 0.0, sumFall = 0.0;

    for (int i = 0; i < kWindowTableSize; ++i)
    {
        sumRise += table[i];
        sumFall += table[kWindowTableSize + i];
    }

    windowMeanRise = static_cast<float> (sumRise / kWindowTableSize);
    windowMeanFall = static_cast<float> (sumFall / kWindowTableSize);
}

//==============================================================================
int64_t PitchFormantShifter::findEpochAnchor (int64_t upTo, float periodSamples) const noexcept
{
    const float* const in48 = rings.getReadPointer (0);

    // 窓幅は周期に比例させる。固定 ms にすると、高い声では周期の大半を覆ってしまい
    // ピークが鈍って位置が決まらない。
    const int half = juce::jlimit (3, 128, juce::roundToInt (periodSamples * 0.125f));
    const int span = juce::jlimit (8, kRingSize / 8, juce::roundToInt (periodSamples));

    // 未来側は 1 サンプルも要らない。half ぶんだけは既に書かれている必要がある。
    const int64_t last = juce::jmin (upTo - 1, writePos48 - half - 1);
    const int64_t first = last - span + 1;

    if (first - half - 1 < 0 || first - half - 1 < readPos48 - kRingSize + 8 || last <= first)
        return upTo - 1;

    double energy = 0.0;

    for (int k = -half; k <= half; ++k)
    {
        const float s = in48[ringIndex (first + k, kRingMask)];
        energy += static_cast<double> (s) * static_cast<double> (s);
    }

    int64_t best = first;
    double  bestEnergy = energy;

    for (int64_t m = first + 1; m <= last; ++m)
    {
        const float added   = in48[ringIndex (m + half, kRingMask)];
        const float removed = in48[ringIndex (m - half - 1, kRingMask)];

        energy += static_cast<double> (added) * static_cast<double> (added)
                    - static_cast<double> (removed) * static_cast<double> (removed);

        if (energy > bestEnergy)
        {
            bestEnergy = energy;
            best = m;
        }
    }

    return best;
}

//==============================================================================
void PitchFormantShifter::reset() noexcept
{
    tracker.reset();
    rings.clear();

    writePos48 = 0;
    readPos48  = -static_cast<int64_t> (kLookaheadSamples);   // 先読みぶんを最初から負で持つ
    nextEpoch  = 0;
    outFrontier = 0;
    lastMark   = 0;
    markValid  = false;

    pitchSmoothedSt   = pitchTargetSt;
    formantSmoothedSt = formantTargetSt;

    wetMix.setCurrentAndTargetValue (wetTarget);

    jitterState = 0x12345678u;
}

//==============================================================================
void PitchFormantShifter::setPitchSemitones (float semitones) noexcept
{
    pitchTargetSt = juce::jlimit (kMinSemitones, kMaxSemitones, semitones);
}

void PitchFormantShifter::setFormantSemitones (float semitones) noexcept
{
    formantTargetSt = juce::jlimit (kMinSemitones, kMaxSemitones, semitones);
}

void PitchFormantShifter::setWetMix (float wet01) noexcept
{
    wetTarget = juce::jlimit (0.0f, 1.0f, wet01);
    wetMix.setTargetValue (wetTarget);
}

//==============================================================================
void PitchFormantShifter::process (const float* input, float* output, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    if (input == nullptr || output == nullptr || numSamples <= 0)
        return;

    float* const in48     = rings.getWritePointer (0);
    float* const outAccum = rings.getWritePointer (1);
    float* const dry      = rings.getWritePointer (2);

    // 入力を先に全部リングへ写す。これで input == output のエイリアシングも成立する。
    for (int i = 0; i < numSamples; ++i)
    {
        const int k = ringIndex (writePos48 + i, kRingMask);
        in48[k] = input[i];
        dry[k]  = input[i];
    }

    tracker.pushBlock (input, numSamples);
    writePos48 += numSamples;

    synthesiseGrains();

    // 出力を取り出しながら、通過したアキュムレータを消す。
    // ここで消すので、リングが一周して戻ってきたときには必ずゼロになっている。
    for (int i = 0; i < numSamples; ++i)
    {
        const int k = ringIndex (readPos48 + i, kRingMask);

        const float wet = outAccum[k];
        const float d   = dry[k];

        outAccum[k] = 0.0f;

        // ドライも同じ先読みぶん遅れているので、この混合で遅延は動かない。
        const float m = wetMix.getNextValue();
        const float v = d + m * (wet - d);

        output[i] = std::isfinite (v) ? v : 0.0f;
    }

    readPos48 += numSamples;
}

//==============================================================================
void PitchFormantShifter::synthesiseGrains() noexcept
{
    const float* const in48     = rings.getReadPointer (0);
    float* const       outAccum = rings.getWritePointer (1);

    float* const       seg      = grain.getWritePointer (0);
    const float* const window   = grain.getReadPointer (1);
    const int          segCapacity = grain.getNumSamples();

    const float* const ring8 = tracker.getDecimatedRing();
    const int          mask8 = PitchTracker::getDecimatedRingMask();
    const int          decim = juce::jmax (1, tracker.getDecimationFactor());

    float* const ref8 = prevSeg.getWritePointer (0);
    const int    refCapacity = prevSeg.getNumSamples();

    // 1 ブロックで回りすぎないための保険。通常は数個しか回らない。
    for (int guard = 0; guard < 64; ++guard)
    {
        const PitchTracker::Result pitch = tracker.getCurrent();

        const float T0 = juce::jlimit (PitchTracker::kMinPeriod48,
                                       PitchTracker::kMaxPeriod48,
                                       pitch.periodSamples48);
        const float v = juce::jlimit (0.0f, 1.0f, pitch.voicing);

        //----------------------------------------------------------------------
        // 半音値の平滑。グレインごとに 1 回だけ評価し、1 グレイン 1 半音に制限する。
        // グレイン間隔は T0 前後なので、一次遅れの係数はそれで導出する。
        {
            const float a = std::exp (-T0 / juce::jmax (1.0f, kParamTauSeconds * static_cast<float> (sampleRate)));

            const float pitchNext = pitchSmoothedSt + (pitchTargetSt - pitchSmoothedSt) * (1.0f - a);
            const float formNext  = formantSmoothedSt + (formantTargetSt - formantSmoothedSt) * (1.0f - a);

            pitchSmoothedSt   += juce::jlimit (-kMaxSemitoneStepPerGrain, kMaxSemitoneStepPerGrain,
                                               pitchNext - pitchSmoothedSt);
            formantSmoothedSt += juce::jlimit (-kMaxSemitoneStepPerGrain, kMaxSemitoneStepPerGrain,
                                               formNext - formantSmoothedSt);
        }

        const float P = std::exp2 (pitchSmoothedSt / 12.0f);
        const float F = std::exp2 (formantSmoothedSt / 12.0f);

        // 無声区間にピッチ間隔を押し付けない。/s/ /sh/ が金属的になるのを防ぐ。
        const float pEff = juce::jmax (0.25f, 1.0f + v * (P - 1.0f));

        // ★v1.0.0 の不具合の核心はここ。
        //   PSOLA でピッチを動かすのは「解析側と合成側で歩幅を変える」ことだけ:
        //     解析マークは入力の自然周期 T0 ごと（= 声門エポックの上）に進み、
        //     出力エポックは Hs = T0 / P_eff ごとに進む。
        //   出力の周期が Hs になるので F0_out = F0_in * P_eff になる。
        //   v1.0.0 は解析マークも Hs で進めていた。歩幅が同じ = WSOLA の恒等変換で、
        //   グレイン内リサンプル（フォルマント）だけが効き、ピッチは一切動かなかった。
        const int T0i = juce::jmax (2, juce::roundToInt (T0));
        const int Hs  = juce::jmax (1, juce::roundToInt (T0 / pEff));

        const int searchRange = juce::jlimit (kSearchFloor, kSearchCap, juce::roundToInt (T0 * 0.1f));

        //----------------------------------------------------------------------
        // グレイン形状。★長さは「入力側」で前後 T0 ずつと決める。
        //
        // 窓は出力格子の上で掛かるが、読む位置は centre + j*F なので、
        // 実際に切り出している入力長は hOut*F になる。ここを出力側で 2*T0 に決めると、
        // F > 1 のとき入力長が 2*T0*F になり、隣の声門パルスまでグレインに入る。
        // 入ったパルスは F 倍に詰まった間隔で並ぶので、フォルマントのつまみが
        // ピッチを動かしてしまう（v1.0.0 の症状のもう半分）。
        // 入力側を T0 に固定すれば、隣のパルスはちょうど窓のゼロ点に載って消える。
        //
        // P には依らない。P >= 0.5（= -12 半音）までは間隔 Hs <= 2*T0 なので、
        // 出力窓 2*T0/F と重ね合わせても隙間は空かない（F <= 2*P_eff の範囲で）。
        //
        // 先読み予算の根拠:
        //   出力位置 t を確定するにはエポック t + hOutLeft を合成する必要があり、
        //   そのためには入力が mark + hInRight まで要る。mark <= エポック + searchRange なので
        //       hOutLeft + hInRight + searchRange <= LOOKAHEAD
        //   が守られていれば、グレインの左裾が「既に出力した領域」へはみ出さない。
        //
        // ★v1.0.0 は窓を非対称（左 1.5*Hs / 右 0.5*Hs）にして右側だけ予算に収めていたが、
        //   左裾も同じだけ遅延を食う（出力を確定できない）ことを見落としていた。
        //   結果 nextEpoch が毎ブロック前へ押し出され、エポック列が成立していなかった。
        //   予算に収まらない場合は入力長・出力長を同じ比率で縮める。中心が動かないので
        //   エポックとの位相関係は保たれ、低い声では「短い窓」に劣化するだけで済む。
        const int budget = juce::jmax (64, kLookaheadSamples - searchRange - kBudgetMargin);

        float halfIn = T0;
        float halfOut = halfIn / juce::jmax (0.1f, F);

        {
            const float cost = halfOut + halfIn;

            if (cost > static_cast<float> (budget))
            {
                const float scale = static_cast<float> (budget) / cost;
                halfIn  *= scale;
                halfOut *= scale;
            }
        }

        const int hOutLeft  = juce::jlimit (kMinHalfOut, kMaxHalfOut, juce::roundToInt (halfOut));
        const int hOutRight = hOutLeft;

        const int hInLeft  = juce::jmax (1, juce::roundToInt (static_cast<float> (hOutLeft)  * F));
        const int hInRight = juce::jmax (1, juce::roundToInt (static_cast<float> (hOutRight) * F));

        const int segLen = hInLeft + hInRight + 4;

        if (segLen > segCapacity)
            break;   // 起こらないはずだが、起きたら合成をやめる（無音より破損の方が悪い）

        //----------------------------------------------------------------------
        // 必要な入力が揃っているか。揃っていなければ次のブロックへ持ち越す。
        // mark <= nextEpoch + searchRange なので、この 1 本で探索の右端まで賄える。
        if (nextEpoch + searchRange + hInRight + 2 > writePos48)
            break;

        // 出力が完全に置き去りになったときだけ再アンカーする（リセット直後など）。
        // 予算が守られていれば通常は成立しない。
        if (nextEpoch + hOutRight <= readPos48)
        {
            nextEpoch = readPos48 + 1;
            markValid = false;
        }

        //----------------------------------------------------------------------
        // トランジェント検出。直近 3 ms が直前 6 ms の 4 倍を超えたら相関探索をやめ、
        // 目標位置そのものを使う（探索でアタックを前後にずらすと二重打ちに聞こえる）。
        const int64_t targetMark = nextEpoch;

        bool transient = false;
        {
            const int recentLen = juce::jmax (1, static_cast<int> (0.003 * sampleRate));
            const int priorLen  = juce::jmax (1, static_cast<int> (0.006 * sampleRate));

            if (targetMark - (recentLen + priorLen) >= 0)
            {
                float eRecent = 0.0f, ePrior = 0.0f;

                for (int i = 0; i < recentLen; ++i)
                {
                    const float s = in48[ringIndex (targetMark - 1 - i, kRingMask)];
                    eRecent += s * s;
                }

                for (int i = 0; i < priorLen; ++i)
                {
                    const float s = in48[ringIndex (targetMark - 1 - recentLen - i, kRingMask)];
                    ePrior += s * s;
                }

                const float meanRecent = eRecent / static_cast<float> (recentLen);
                const float meanPrior  = ePrior  / static_cast<float> (priorLen);

                transient = meanRecent > kTransientRatio * meanPrior && meanRecent > 1.0e-9f;
            }
        }

        //----------------------------------------------------------------------
        // 解析マークの候補位置。声門エポック列 lastMark + n*T0 の上を進み、
        // 出力エポックを追い越さない最大の n を採る。
        //   n == 0  同じ解析グレインをもう一度出す（ピッチを上げたとき）
        //   n == 1  ふつうに 1 周期ぶん進む
        //   n >= 2  解析グレインを間引く（ピッチを下げたとき）
        // 絶対位置から毎回計算し直すので、解析側と合成側の時間がずれ続けることはない。
        int advance = 1;
        int64_t markCentre = targetMark;

        if (markValid)
        {
            advance = static_cast<int> (floorDiv (targetMark - lastMark, T0i));
            advance = juce::jlimit (0, kMaxAdvance, advance);
            markCentre = lastMark + static_cast<int64_t> (advance) * T0i;
        }

        const bool voiced = v >= kUnvoicedVoicing;

        int64_t mark = markCentre;

        if (advance == 0 && markValid && ! transient)
        {
            // 複製。同じ場所が最良なので探索する意味がない。
            mark = lastMark;
        }
        else if (voiced)
        {
            // ★位相アンカー。毎グレイン、中心を声門閉鎖点に載せ直す。
            //   markCentre から過去 1 周期の範囲には声門パルスがちょうど 1 つある。
            //   中心がパルスに載っていれば、隣のパルスはちょうど窓のゼロ点（±T0）に
            //   来て消える。載っていないと隣のパルスが窓の両端で半分ずつ生き残り、
            //   ピッチを下げても元の高さが残る（v1.0.0 の症状）。
            //   相関ロックだけでは「前のグレインとの相対位相」しか保てず、
            //   最初にずれた位相はそのまま固定されてしまうので、毎回載せ直す。
            //   探索は過去向きだけなので先読みは 1 サンプルも増えない。
            mark = findEpochAnchor (markCentre + 1, T0);
        }
        else
        {
            // 無声区間。エネルギーのピークには意味が無いので相関でつなぐ。
            // 参照は「前の解析マーク」。
            // ★ここを出力側の連続性（lastMark + Hs）に取ると WSOLA になり、
            //   歩幅が合成側に引きずられてピッチが動かなくなる（v1.0.0 の症状）。
            const int corrLen8 = juce::jlimit (12, juce::jmin (refCapacity, 128),
                                               static_cast<int> (T0 / static_cast<float> (decim)));
            const int search8  = juce::jmax (1, searchRange / decim);

            const int64_t refEnd8 = floorDiv (lastMark, decim);
            const int64_t centre8 = floorDiv (markCentre, decim);

            const int64_t oldest8 = tracker.getDecimatedWritePos() - PitchTracker::getDecimatedRingSize() + 8;

            if (refEnd8 - corrLen8 >= oldest8
                && centre8 - search8 - corrLen8 >= oldest8)
            {
                for (int i = 0; i < corrLen8; ++i)
                    ref8[i] = ring8[ringIndex (refEnd8 - corrLen8 + i, mask8)];

                float bestScore = -2.0f;
                int   bestDelta8 = 0;

                for (int d = -search8; d <= search8; ++d)
                {
                    const int64_t candEnd8 = centre8 + d;

                    float xy = 0.0f, xx = 0.0f, yy = 0.0f;

                    for (int i = 0; i < corrLen8; ++i)
                    {
                        const float a = ring8[ringIndex (candEnd8 - corrLen8 + i, mask8)];
                        const float b = ref8[i];

                        xy += a * b;
                        xx += a * a;
                        yy += b * b;
                    }

                    const float denom = std::sqrt (xx * yy);
                    const float score = denom > 1.0e-12f ? xy / denom : 0.0f;

                    if (score > bestScore)
                    {
                        bestScore  = score;
                        bestDelta8 = d;
                    }
                }

                mark = markCentre + static_cast<int64_t> (bestDelta8) * decim;

                // 48 kHz で ±3 サンプル追い込む。8 kHz 格子のままだと 6 サンプル刻みの
                // 位相ずれが残り、伸ばした母音でわずかにざらつく。
                const int fineLen = juce::jlimit (32, juce::jmin (refCapacity, 256), static_cast<int> (T0));

                if (lastMark - fineLen >= 0 && mark - fineLen - kFineRefine >= 0)
                {
                    float* const ref48 = prevSeg.getWritePointer (1);

                    for (int i = 0; i < fineLen; ++i)
                        ref48[i] = in48[ringIndex (lastMark - fineLen + i, kRingMask)];

                    float bestFine = -2.0f;
                    int   bestOffset = 0;

                    for (int d = -kFineRefine; d <= kFineRefine; ++d)
                    {
                        float xy = 0.0f, xx = 0.0f, yy = 0.0f;

                        for (int i = 0; i < fineLen; ++i)
                        {
                            const float a = in48[ringIndex (mark + d - fineLen + i, kRingMask)];
                            const float b = ref48[i];

                            xy += a * b;
                            xx += a * a;
                            yy += b * b;
                        }

                        const float denom = std::sqrt (xx * yy);
                        const float score = denom > 1.0e-12f ? xy / denom : 0.0f;

                        if (score > bestFine)
                        {
                            bestFine   = score;
                            bestOffset = d;
                        }
                    }

                    mark += bestOffset;
                }
            }

            // 無声ぶんのジッタ。周期性を持たない音に規則正しい間隔を与えないため。
            const float jitter = nextJitter (jitterState) * (1.0f - v) * (T0 * 0.125f);
            mark += static_cast<int64_t> (juce::roundToInt (jitter));

            // 探索とジッタで候補位置から離れすぎないように最終クランプ。
            mark = juce::jlimit (markCentre - searchRange, markCentre + searchRange, mark);
        }

        if (mark + hInRight + 2 > writePos48)
            mark = writePos48 - hInRight - 2;

        if (mark - hInLeft - 1 < readPos48 - kRingSize + 8)
            break;   // 過去側がリングから溢れた（極端に低い T0）。次ブロックで作り直す。

        //----------------------------------------------------------------------
        // グレイン素材を連続バッファへ。ここで一度写すと、補間の内側でリング境界を
        // 気にしなくてよくなり、分岐が消える。
        const int64_t segStart = mark - hInLeft - 1;

        for (int k = 0; k < segLen; ++k)
            seg[k] = in48[ringIndex (segStart + k, kRingMask)];

        //----------------------------------------------------------------------
        // フォルマントを上げる = グレインを速く読む = 折り返す。読む前に落とす。
        // F <= 1.05 のときは不要（むしろ高域を無駄に削る）。
        if (F > kAntiAliasThreshold)
        {
            const double fc = 0.45 * sampleRate / static_cast<double> (F);
            const double w0 = 2.0 * 3.14159265358979323846 * fc / sampleRate;
            const double cosW = std::cos (w0);
            const double alpha = std::sin (w0) / (2.0 * 0.70710678118654752440);

            const double a0 =  1.0 + alpha;
            const float  b0 = static_cast<float> (((1.0 - cosW) * 0.5) / a0);
            const float  b1 = static_cast<float> ((1.0 - cosW) / a0);
            const float  b2 = b0;
            const float  a1 = static_cast<float> ((-2.0 * cosW) / a0);
            const float  a2 = static_cast<float> ((1.0 - alpha) / a0);

            // 端は窓でゼロに落ちるので、フィルタの立上りは聞こえない。
            float x1 = seg[0], x2 = seg[0];
            float y1 = seg[0], y2 = seg[0];

            for (int k = 0; k < segLen; ++k)
            {
                const float x0 = seg[k];
                const float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

                x2 = x1; x1 = x0;
                y2 = y1; y1 = y0;

                seg[k] = y0;
            }
        }

        //----------------------------------------------------------------------
        // OLA。窓は「リサンプル後」の出力格子の上に掛ける。
        //
        // ★ゲインはグレインごとに解析的に決める（v1.0.0 は出力サンプルごとに
        //   積算した窓の和 winAccum で割っていた）。
        //   点ごとに窓の和で割るということは、重なりが薄い場所ほど大きく持ち上げる
        //   ということで、極端には重なりゼロの場所で outAccum/winAccum = w*x/w = x、
        //   つまり窓が完全に打ち消される。窓の役目は「隣の声門パルスを ±T0 の
        //   ゼロ点で消すこと」なので、これが打ち消されると隣のパルスが素通りし、
        //   -12 半音にしても元の高さがそのまま残る。
        //   重なり数 = 2*halfOut/Hs は設定で 1〜4 倍まで動くので、
        //   窓の積分値 / Hs で割れば、どの設定でも平均レベルは 1 に揃う。
        const float windowSum = static_cast<float> (hOutLeft) * windowMeanRise
                                    + static_cast<float> (hOutRight) * windowMeanFall;

        const float grainGain = static_cast<float> (Hs) / juce::jmax (1.0f, windowSum);

        const float centre = static_cast<float> (hInLeft + 1);
        const float invLeft  = static_cast<float> (kWindowTableSize) / static_cast<float> (hOutLeft);
        const float invRight = static_cast<float> (kWindowTableSize) / static_cast<float> (hOutRight);

        for (int j = -hOutLeft; j < hOutRight; ++j)
        {
            // 既に出力し終えた領域には書かない。書いてもその音は誰も読まず、
            // リングが一周したときにゴミとして戻ってくるだけ。
            if (nextEpoch + j < readPos48)
                continue;

            float w;

            if (j < 0)
            {
                const float t = static_cast<float> (j + hOutLeft) * invLeft;
                const int   i0 = juce::jlimit (0, kWindowTableSize - 2, static_cast<int> (t));
                const float fr = t - static_cast<float> (i0);
                w = window[i0] + fr * (window[i0 + 1] - window[i0]);
            }
            else
            {
                const float t = static_cast<float> (j) * invRight;
                const int   i0 = juce::jlimit (0, kWindowTableSize - 2, static_cast<int> (t));
                const float fr = t - static_cast<float> (i0);
                w = window[kWindowTableSize + i0]
                        + fr * (window[kWindowTableSize + i0 + 1] - window[kWindowTableSize + i0]);
            }

            const float pos = centre + static_cast<float> (j) * F;

            int   i0 = static_cast<int> (pos);
            float fr = pos - static_cast<float> (i0);

            i0 = juce::jlimit (1, segLen - 3, i0);

            const float s = hermite (seg[i0 - 1], seg[i0], seg[i0 + 1], seg[i0 + 2], fr);

            const int k = ringIndex (nextEpoch + j, kRingMask);

            outAccum[k] += grainGain * w * s;
        }

        outFrontier = juce::jmax (outFrontier, nextEpoch + hOutRight);

        lastMark  = mark;
        markValid = true;
        nextEpoch += Hs;
    }
}

} // namespace kvc
