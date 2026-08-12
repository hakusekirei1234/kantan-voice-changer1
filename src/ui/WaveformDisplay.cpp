// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "ui/WaveformDisplay.h"

#include <cmath>

namespace kvc
{

namespace
{
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    // WaveSpectra 風の配色。Main.cpp の kvc::Colours とは別に持つ
    // （あちらは Main.cpp のローカル定義で、ここからは見えない）。
    const juce::Colour kField        { 0xff000000 };
    const juce::Colour kGridBase     { 0xff00ff40 };
    const juce::Colour kAxisText     { 0xff3e7a50 };
    const juce::Colour kTrace        { 0xff2eff7a };
    const juce::Colour kTraceMuted   { 0xff4a5057 };
    const juce::Colour kClip         { 0xffff4757 };
    const juce::Colour kCaption      { 0xff4a9060 };
    const juce::Colour kLatencyOk    { 0xff8a9099 };
    const juce::Colour kLatencyWarn  { 0xffffa502 };
    const juce::Colour kMeterBack    { 0xff16191d };
    const juce::Colour kMeterTrack   { 0xff23282e };
    const juce::Colour kMeterGreen   { 0xff2ed573 };
    const juce::Colour kMeterAmber   { 0xffffa502 };

    constexpr int   kVerticalDivisions = 10;
    constexpr int   kHorizontalDivisions = 8;
    constexpr float kTraceInsetPx = 3.0f;

    /** レベルメーターの目盛り。振幅ではなく dB 位置で描かないと、
        普通の話し声がバーの左端 1/10 に張り付いて動いて見えない。
        juce::Decibels は juce_audio_basics 側なので、ここでは使わずに自前で出す
        （このファイルは juce_gui_basics だけに依存させておきたい）。 */
    constexpr float kMeterFloorDb = -54.0f;
    constexpr float kMeterAmberAmplitude = 0.5011872f;   // -6 dBFS

    float meterPositionFor (float amplitude) noexcept
    {
        if (amplitude <= 1.0e-6f)
            return 0.0f;

        const float db = 20.0f * std::log10 (amplitude);
        return juce::jlimit (0.0f, 1.0f, (db - kMeterFloorDb) / -kMeterFloorDb);
    }
}

//==============================================================================
WaveformDisplay::WaveformDisplay (PeakRing& ringToUse, RawRing& rawToUse)
    : peaks (ringToUse), raw (rawToUse)
{
    // これを外すと毎フレーム背後の親まで再描画され、静かに 10〜15% の CPU を食う。
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
}

WaveformDisplay::~WaveformDisplay() = default;

//==============================================================================
void WaveformDisplay::setMuted (bool shouldBeMuted, const juce::String& keyName)
{
    if (muted == shouldBeMuted && unmuteKeyName == keyName)
        return;

    muted = shouldBeMuted;

    if (keyName.isNotEmpty())
        unmuteKeyName = keyName;

    repaint();
}

void WaveformDisplay::setLatencyText (const juce::String& text, bool warn)
{
    if (latencyText == text && latencyWarn == warn)
        return;

    latencyText = text;
    latencyWarn = warn;
    repaint();
}

void WaveformDisplay::setSampleRate (double newRate)
{
    if (newRate <= 0.0 || std::abs (newRate - sampleRate) < 1.0)
        return;

    sampleRate = newRate;
    peaks.setDecimation (PeakRing::suggestDecimation (sampleRate, windowSeconds, getWidth()));
}

void WaveformDisplay::setWindowSeconds (double seconds)
{
    seconds = juce::jlimit (0.1, 4.0, seconds);

    if (std::abs (seconds - windowSeconds) < 1.0e-6)
        return;

    windowSeconds = seconds;
    peaks.setDecimation (PeakRing::suggestDecimation (sampleRate, windowSeconds, getWidth()));
    rebuildGrid();
    repaint();
}

//==============================================================================
void WaveformDisplay::resized()
{
    rebuildGrid();

    peaks.setDecimation (PeakRing::suggestDecimation (sampleRate, windowSeconds, getWidth()));

    // 列は 1 px 幅ぶん 1 個。確保はここだけで済ませ、paint では clearQuick しかしない。
    traceRects.ensureStorageAllocated (juce::jmax (256, getWidth() + 8));
    clipRects .ensureStorageAllocated (juce::jmax (128, getWidth() / 4 + 8));

    updateAnimationState();
}

void WaveformDisplay::visibilityChanged()        { updateAnimationState(); }
void WaveformDisplay::parentHierarchyChanged()   { updateAnimationState(); }

void WaveformDisplay::updateAnimationState()
{
    const bool shouldRun = isShowing() && getPeer() != nullptr && getWidth() > 0;

    if (shouldRun == ! vblank.isEmpty())
        return;

    if (shouldRun)
        vblank = juce::VBlankAttachment (this, [this] { repaint(); });
    else
        vblank = {};   // トレイ最小化中に GPU/CPU を 0% にする唯一の方法
}

//==============================================================================
void WaveformDisplay::rebuildGrid()
{
    if (getWidth() <= 0 || getHeight() <= 0)
    {
        gridImage = {};
        return;
    }

    float scale = juce::Desktop::getInstance().getGlobalScaleFactor();

    if (auto* peer = getPeer())
        scale *= (float) peer->getPlatformScaleFactor();

    scale = juce::jlimit (0.5f, 4.0f, scale);

    gridImage = juce::Image (juce::Image::RGB,
                             juce::roundToInt ((float) getWidth()  * scale),
                             juce::roundToInt ((float) getHeight() * scale),
                             false);

    juce::Graphics g (gridImage);
    g.addTransform (juce::AffineTransform::scale (scale));

    const float w = (float) getWidth();
    const float h = (float) getHeight();

    g.fillAll (kField);

    const auto minor  = kGridBase.withAlpha (0.13f);
    const auto major  = kGridBase.withAlpha (0.30f);
    const auto border = kGridBase.withAlpha (0.35f);

    // 縦線は周波数（対数）。倍音がどの高さに並んでいるかを読むための目盛りなので、
    // 等間隔ではなく 1-2-5 系列で引く。
    static const float gridHz[] = { 50, 100, 200, 300, 500, 700,
                                    1000, 2000, 3000, 5000, 7000, 10000, 15000 };

    for (const float hz : gridHz)
    {
        if (hz < kMinHz || hz > kMaxHz)
            continue;

        const float x = std::floor (w * xForHz (hz));
        const bool  isMajor = (hz == 100.0f || hz == 1000.0f || hz == 10000.0f);

        g.setColour (isMajor ? major : minor);
        g.fillRect (x, 0.0f, 1.0f, h);
    }

    // 横線は dB。12 dB ごと。
    for (int db = -12; db > (int) kFloorDb; db -= 12)
    {
        const float y = std::floor (h * (kTopDb - (float) db) / (kTopDb - kFloorDb));
        g.setColour ((db % 24) == 0 ? major : minor);
        g.fillRect (0.0f, y, w, 1.0f);
    }

    g.setColour (border);
    g.fillRect (0.0f, 0.0f, w, 1.0f);
    g.fillRect (0.0f, h - 1.0f, w, 1.0f);
    g.fillRect (0.0f, 0.0f, 1.0f, h);
    g.fillRect (w - 1.0f, 0.0f, 1.0f, h);

    g.setColour (kAxisText);
    g.setFont (juce::Font (juce::FontOptions (10.5f)));

    // 周波数のラベル。読めれば十分なので主要どころだけ。
    struct { float hz; const char* text; } labels[] =
    {
        { 100.0f, "100" }, { 500.0f, "500" }, { 1000.0f, "1k" },
        { 3000.0f, "3k" }, { 10000.0f, "10k" }
    };

    for (const auto& l : labels)
    {
        const float x = w * xForHz (l.hz);
        g.drawText (l.text, juce::Rectangle<float> (x - 24.0f, h - 15.0f, 48.0f, 13.0f),
                    juce::Justification::centred, false);
    }

    g.drawText ("Hz", juce::Rectangle<float> (w - 34.0f, h - 15.0f, 28.0f, 13.0f),
                juce::Justification::centredRight, false);

    for (int db = -24; db > (int) kFloorDb; db -= 24)
        g.drawText (juce::String (db),
                    juce::Rectangle<float> (3.0f,
                                            h * (kTopDb - (float) db) / (kTopDb - kFloorDb) - 7.0f,
                                            30.0f, 13.0f),
                    juce::Justification::centredLeft, false);
}

float WaveformDisplay::xForHz (float hz) const noexcept
{
    const float lo = std::log (kMinHz);
    const float hi = std::log (kMaxHz);

    return juce::jlimit (0.0f, 1.0f, (std::log (juce::jmax (1.0f, hz)) - lo) / (hi - lo));
}

//==============================================================================
void WaveformDisplay::paint (juce::Graphics& g)
{
    if (gridImage.isValid())
    {
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (gridImage, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
    }
    else
    {
        g.fillAll (kField);
    }

    paintTrace (g);

    const float w = (float) getWidth();
    const float h = (float) getHeight();

    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.setColour (kCaption);
    g.drawText (jp ("マイクの周波数（倍音）"),
                juce::Rectangle<float> (8.0f, h - 26.0f, 160.0f, 14.0f),
                juce::Justification::centredLeft, false);

    if (latencyText.isNotEmpty())
    {
        // 右下は周波数軸のラベル（10k / Hz）と重なるので右上に置く。
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.setColour (latencyWarn ? kLatencyWarn : kLatencyOk);
        g.drawText (latencyText,
                    juce::Rectangle<float> (w - 248.0f, 6.0f, 240.0f, 16.0f),
                    juce::Justification::centredRight, false);
    }

    if (muted)
    {
        g.setColour (kClip);
        g.drawRect (getLocalBounds().toFloat(), 2.0f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
        g.drawText (jp ("ミュート中（") + unmuteKeyName + jp (" キーで解除）"),
                    getLocalBounds().toFloat(), juce::Justification::centred, false);
    }
}

void WaveformDisplay::updateSpectrum()
{
    // 直近 1 フレームぶんを取り出して窓を掛け、振幅スペクトルにする。
    raw.readLatest (fftBuffer.data(), kFftSize);
    std::fill (fftBuffer.begin() + kFftSize, fftBuffer.end(), 0.0f);

    window.multiplyWithWindowingTable (fftBuffer.data(), (size_t) kFftSize);
    fft.performFrequencyOnlyForwardTransform (fftBuffer.data());

    // ハン窓のコヒーレントゲイン 0.5 と FFT 長で正規化して、
    // フルスケール正弦波が 0 dB になるようにする。
    const float norm = 2.0f / ((float) kFftSize * 0.5f);

    // 立ち上がりは速く、落ちはゆっくり。生の値だとちらついて倍音が読めない。
    constexpr float kRise = 0.55f;
    constexpr float kFall = 0.12f;
    constexpr float kPeakFallDb = 0.9f;      // 1 フレームあたり

    for (int i = 0; i < kNumBins; ++i)
    {
        const float mag = fftBuffer[(size_t) i] * norm;
        const float db  = juce::jmax (kFloorDb, juce::Decibels::gainToDecibels (mag, kFloorDb));

        float& s = binDb[(size_t) i];
        s += (db > s ? kRise : kFall) * (db - s);

        float& p = binPeak[(size_t) i];
        p = s > p ? s : juce::jmax (kFloorDb, p - kPeakFallDb);
    }
}

void WaveformDisplay::paintTrace (juce::Graphics& g)
{
    const int width = getWidth();
    const float h   = (float) getHeight();

    if (width <= 0 || h <= 4.0f)
        return;

    updateSpectrum();

    const float nyquist = (float) (sampleRate * 0.5);
    const float binHz   = nyquist / (float) kNumBins;

    const auto yForDb = [h] (float db)
    {
        return juce::jlimit (0.0f, h - 1.0f,
                             h * (kTopDb - db) / (kTopDb - kFloorDb));
    };

    // 横 1 px ごとに、その位置に対応する周波数帯のビンの最大値を取る。
    // 対数軸なので低域は 1 ビットが数 px に伸び、高域は数十ビンが 1 px に潰れる。
    // 最大値を採らないと倍音のピークが平均で消える。
    tracePath.clear();
    peakPath .clear();

    float prevHz = kMinHz;

    for (int x = 0; x < width; ++x)
    {
        const float t0 = (float) x / (float) width;
        const float t1 = (float) (x + 1) / (float) width;

        const float hz0 = prevHz;
        const float hz1 = kMinHz * std::pow (kMaxHz / kMinHz, t1);
        prevHz = hz1;

        juce::ignoreUnused (t0);

        int a = (int) std::floor (hz0 / binHz);
        int b = (int) std::ceil  (hz1 / binHz);

        a = juce::jlimit (1, kNumBins - 1, a);
        b = juce::jlimit (a + 1, kNumBins, b);

        float db = kFloorDb, pk = kFloorDb;

        for (int i = a; i < b; ++i)
        {
            db = juce::jmax (db, binDb[(size_t) i]);
            pk = juce::jmax (pk, binPeak[(size_t) i]);
        }

        const float fx = (float) x;

        if (x == 0)
        {
            tracePath.startNewSubPath (fx, yForDb (db));
            peakPath .startNewSubPath (fx, yForDb (pk));
        }
        else
        {
            tracePath.lineTo (fx, yForDb (db));
            peakPath .lineTo (fx, yForDb (pk));
        }
    }

    // ピークホールドは細く暗く。倍音の並びを残像で見せるためのもの。
    g.setColour ((muted ? kTraceMuted : kTrace).withAlpha (0.35f));
    g.strokePath (peakPath, juce::PathStrokeType (1.0f));

    g.setColour (muted ? kTraceMuted : kTrace);
    g.strokePath (tracePath, juce::PathStrokeType (1.4f));
}

//==============================================================================
LevelMeter::LevelMeter (PeakRing& ringToUse)
    : peaks (ringToUse)
{
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
    lastClipCount = peaks.getClipCount();
    startTimerHz (30);
}

LevelMeter::~LevelMeter()
{
    stopTimer();
}

void LevelMeter::timerCallback()
{
    const float previousLevel = displayLevel;
    const float previousHold  = peakHold;
    const bool  previousClip  = clipRecent;

    // 瞬時アタック / -20 dB/s リリース。30 Hz なので 1 tick あたり 10^(-1/30)。
    static constexpr float kReleasePerTick = 0.926118f;

    const float instant = juce::jlimit (0.0f, 4.0f, peaks.getMeterPeak());

    displayLevel = juce::jmax (instant, displayLevel * kReleasePerTick);

    if (displayLevel >= peakHold)
    {
        peakHold = displayLevel;
        peakHoldCountdown = 45;          // 1.5 秒ホールド
    }
    else if (--peakHoldCountdown <= 0)
    {
        peakHoldCountdown = 0;
        peakHold = juce::jmax (displayLevel, peakHold * kReleasePerTick);
    }

    const uint32_t clips = peaks.getClipCount();

    if (clips != lastClipCount)
    {
        lastClipCount = clips;
        clipCountdown = 60;              // 直近 2 秒
    }
    else if (clipCountdown > 0)
    {
        --clipCountdown;
    }

    clipRecent = clipCountdown > 0;

    if (std::abs (displayLevel - previousLevel) > 0.0005f
     || std::abs (peakHold - previousHold) > 0.0005f
     || clipRecent != previousClip)
        repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    g.fillAll (kMeterBack);

    const auto bounds = getLocalBounds().toFloat();

    g.setColour (kMeterTrack);
    g.fillRoundedRectangle (bounds, 2.0f);

    const float w = bounds.getWidth();
    const float fill = meterPositionFor (displayLevel) * w;

    if (fill > 0.5f)
    {
        const float amberStart = meterPositionFor (kMeterAmberAmplitude) * w;

        g.setColour (kMeterGreen);
        g.fillRect (bounds.withWidth (juce::jmin (fill, amberStart)));

        if (fill > amberStart)
        {
            g.setColour (kMeterAmber);
            g.fillRect (bounds.withLeft (amberStart).withRight (fill));
        }
    }

    if (clipRecent)
    {
        g.setColour (kClip);
        g.fillRect (bounds.withLeft (juce::jmax (0.0f, w - 4.0f)));
    }

    const float holdX = meterPositionFor (peakHold) * w;

    if (holdX > 1.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.fillRect (juce::jmin (holdX, w - 2.0f), bounds.getY(), 2.0f, bounds.getHeight());
    }
}

} // namespace kvc
