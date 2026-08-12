// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <vector>

#include "core/PeakRing.h"
#include "core/RawRing.h"

namespace kvc
{

//==============================================================================
/** ダークテーマの時間軸波形。表示するのは「入力（マイク）」1 本だけ。

    WaveSpectra の見た目（黒地・細い緑グリッド・緑のトレース・クリップは赤）は
    踏襲するが、アナライザの意味論は持たない。dB 目盛り・周波数軸・THD/RMS/OVL は
    付けない（付けると嘘になる。振幅は線形）。

    性能のための必須事項（どれを外しても 60 fps で 10〜15% CPU を静かに食う）:
      - setOpaque(true)。しないと毎フレーム背後の親まで再描画される。
      - グリッドは resized() で juce::Image に焼き、paint() では 1 回ブリットするだけ。
      - トレースは juce::RectangleList<float> + addWithoutMerging + fillRectList。
        add() は挿入ごとに O(n) のマージを走らせるので使わない。
      - juce::VBlankAttachment で駆動する（Timer だと合成器が捨てるフレームまで描く）。
      - 非表示・トレイ最小化時は VBlankAttachment を破棄して完全に止める。

    ミュート中: 2px の赤枠 + 灰色のトレース + 中央に「ミュート中（○ キーで解除）」。
    トレースは止めない。ミュート中でもマイクが生きていることを見せるため。

    スレッド契約: 全メソッドがメッセージスレッド専用。PeakRing からは
    acquire ロードで書込カーソルの後ろだけを読む。
*/
class WaveformDisplay final : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    static constexpr double kDefaultWindowSeconds = 1.0;

    /** FFT の点数。48 kHz で 11.7 Hz 刻み。男声の基本波（100〜120 Hz）の
        倍音が 1 本ずつ分離して見える最低ライン。これより粗いと倍音が団子になる。 */
    static constexpr int kFftOrder = 12;          // 4096
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kNumBins = kFftSize / 2;

    static constexpr float kFloorDb = -96.0f;
    static constexpr float kTopDb = 0.0f;
    static constexpr float kMinHz = 40.0f;
    static constexpr float kMaxHz = 16000.0f;

    WaveformDisplay (PeakRing&, RawRing&);
    ~WaveformDisplay() override;

    /** ミュート表示。解除キー名は describeBinding() の結果を渡す。 */
    void setMuted (bool shouldBeMuted, const juce::String& unmuteKeyName);

    /** 右下に出す「遅延 約 ○○ ms」。25 ms を超えたら琥珀色にする。 */
    void setLatencyText (const juce::String& text, bool warn);

    /** サンプルレートが変わったらデシメーションを再計算する必要がある。 */
    void setSampleRate (double);

    void setWindowSeconds (double);

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

private:
    void updateAnimationState();
    void rebuildGrid();
    void paintTrace (juce::Graphics&);
    void updateSpectrum();

    /** 周波数 → 0..1 の横位置（対数）。 */
    float xForHz (float hz) const noexcept;

    PeakRing& peaks;
    RawRing&  raw;

    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kFftSize,
                                                 juce::dsp::WindowingFunction<float>::hann };

    /** fft.performFrequencyOnlyForwardTransform は入力を 2 倍長で使う。 */
    std::vector<float> fftBuffer { std::vector<float> ((size_t) kFftSize * 2, 0.0f) };

    /** 表示用に平滑した dB 値と、ゆっくり落ちるピークホールド。
        平滑しないとフレームごとに激しくちらついて倍音が読み取れない。 */
    std::vector<float> binDb   { std::vector<float> ((size_t) kNumBins, kFloorDb) };
    std::vector<float> binPeak { std::vector<float> ((size_t) kNumBins, kFloorDb) };

    juce::Image gridImage;
    juce::Path  tracePath, peakPath;
    juce::RectangleList<float> traceRects, clipRects;
    /** 表示されていない間は default 構築のものを代入して完全に止める
        （isEmpty() == true が「止まっている」状態）。 */
    juce::VBlankAttachment vblank;

    double sampleRate = 48000.0;
    double windowSeconds = kDefaultWindowSeconds;

    bool muted = false;
    juce::String unmuteKeyName { "Ctrl+Shift+M" };
    juce::String latencyText;
    bool latencyWarn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};

//==============================================================================
/** 入力レベルメーター。波形とは別に 30 Hz で再描画する。
    弾道は GUI 側で作る（瞬時アタック、-20 dB/s リリース、ピークホールド）。 */
class LevelMeter final : public juce::Component,
                         private juce::Timer
{
public:
    explicit LevelMeter (PeakRing&);
    ~LevelMeter() override;

    /** 直近 2 秒でクリップしたか。UI は「音が大きすぎます」を出す。 */
    bool isClippingRecently() const noexcept { return clipRecent; }

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    PeakRing& peaks;
    float displayLevel = 0.0f;
    float peakHold = 0.0f;
    int   peakHoldCountdown = 0;
    uint32_t lastClipCount = 0;
    int   clipCountdown = 0;
    bool  clipRecent = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace kvc
