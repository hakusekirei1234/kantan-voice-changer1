// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "ui/MainComponent.h"

#include <cmath>
#include <functional>


namespace
{
    // JUCE の String(const char*) は ASCII 専用（CharPointer_ASCII）。
    // UTF-8 の日本語リテラルは必ずこれを通す。通さないと 1 バイトずつ別文字に化ける。
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

namespace kvc
{

//==============================================================================
namespace
{

// ui.json のカラーパレット。Main.cpp の kvc::Colours とは別実体にしてある
// （inline 変数を 2 つの TU で定義し直すのを避けるため）。
const juce::Colour cBackground { 0xff16191d };
const juce::Colour cPanel      { 0xff1e2227 };
const juce::Colour cPanelHi    { 0xff262b31 };
const juce::Colour cAccent     { 0xff2ed573 };
const juce::Colour cDanger     { 0xffff4757 };
const juce::Colour cText       { 0xffe8eaed };
const juce::Colour cTextDim    { 0xff8a9099 };

/** JUCE 8 では juce::Font(name,size,style) が deprecated。FontOptions を通す。
    書体名は指定しない。ja-JP の Windows では既定の UI 書体（Yu Gothic UI 等）が
    選ばれ、日本語が正しく出る。名前を固定すると化ける機械が出る。 */
juce::Font uiFont (float height, bool bold = false)
{
    auto options = juce::FontOptions().withHeight (height);

    if (bold)
        options = options.withStyle ("Bold");

    return juce::Font (options);
}

juce::String formatMmSs (double seconds)
{
    if (! (seconds > 0.0))
        seconds = 0.0;

    const int total = static_cast<int> (seconds);
    return juce::String (total / 60).paddedLeft ('0', 2)
         + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}

juce::String signedSemitones (float value)
{
    const int v = juce::roundToInt (value);
    return (v > 0 ? "+" : "") + juce::String (v) + jp (" 半音");
}

void styleLabel (juce::Label& label, const juce::String& text, float height,
                 juce::Colour colour, bool bold = false,
                 juce::Justification justification = juce::Justification::centredLeft)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (uiFont (height, bold));
    label.setColour (juce::Label::textColourId, colour);
    label.setJustificationType (justification);
    label.setInterceptsMouseClicks (false, false);
}

void styleButton (juce::TextButton& button, juce::Colour background, juce::Colour textColour)
{
    button.setColour (juce::TextButton::buttonColourId, background);
    button.setColour (juce::TextButton::buttonOnColourId, background);
    button.setColour (juce::TextButton::textColourOffId, textColour);
    button.setColour (juce::TextButton::textColourOnId, textColour);
}

void styleToggle (juce::ToggleButton& toggle, const juce::String& text)
{
    toggle.setButtonText (text);
    toggle.setColour (juce::ToggleButton::textColourId, cText);
    toggle.setColour (juce::ToggleButton::tickColourId, cAccent);
    toggle.setColour (juce::ToggleButton::tickDisabledColourId, cTextDim);
}

//==============================================================================
/** ステータスバーの一時メッセージ。保持期限を Component のプロパティに置くことで
    ヘッダにメンバを増やさずに済ませている（4 Hz のタイマーが毎回上書きするため、
    期限が無いと「保存しました」が一瞬で消える）。 */
constexpr const char* kStickyKey = "statusStickyUntilMs";
constexpr const char* kXrunKey   = "lastKnownXruns";

void setStatusText (juce::Label& label, const juce::String& text, bool isError)
{
    label.setText (text, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, isError ? cDanger : cTextDim);
}

void showTransient (juce::Component& owner, juce::Label& label,
                    const juce::String& text, bool isError, int holdMs)
{
    setStatusText (label, text, isError);
    owner.getProperties().set (kStickyKey, juce::Time::currentTimeMillis() + holdMs);
}

bool transientStillShowing (const juce::Component& owner)
{
    return juce::Time::currentTimeMillis()
             < static_cast<juce::int64> (owner.getProperties().getWithDefault (kStickyKey, juce::var (juce::int64 (0))));
}

//==============================================================================
juce::String describeLeg (const AudioEngine::LegStatus& leg)
{
    if (! leg.active)
        return jp ("未使用");

    return juce::String (juce::roundToInt (leg.sampleRate)) + " Hz / "
         + juce::String (leg.blockSize) + jp (" サンプル / 接続中 ")
         + backendBadge (leg.backend);
}

void updateDeviceStatusLines (DeviceSelectorPanel& panel,
                              const AudioEngine::Status& status,
                              bool sendSelected)
{
    using Column = DeviceSelectorPanel::Column;

    if (! status.running)
    {
        const bool hasError = status.lastErrorJapanese.isNotEmpty();
        panel.setStatusLine (Column::input,   hasError ? status.lastErrorJapanese : juce::String (jp ("停止中")), hasError);
        panel.setStatusLine (Column::monitor, jp ("停止中"), false);
        panel.setStatusLine (Column::send,
                             sendSelected ? juce::String (jp ("停止中"))
                                          : juce::String (jp ("送信しません（モニターのみ）")),
                             false);
        return;
    }

    panel.setStatusLine (Column::input, describeLeg (status.input), ! status.input.active);

    if (status.collapsed)
        panel.setStatusLine (Column::monitor, jp ("入力と同じ機器（いちばん遅れが少ない状態です）"), false);
    else
        panel.setStatusLine (Column::monitor, describeLeg (status.monitor), ! status.monitor.active);

    if (! sendSelected)
        panel.setStatusLine (Column::send, jp ("送信しません（モニターのみ）"), false);
    else
        panel.setStatusLine (Column::send, describeLeg (status.send), ! status.send.active);
}

//==============================================================================
/** トレイ用の 32x32 マイクアイコン。画像資産は持たない（合成のみ）。 */
juce::Image makeTrayIcon (bool muted)
{
    juce::Image image (juce::Image::ARGB, 32, 32, true);
    juce::Graphics g (image);

    const auto body = muted ? juce::Colour (0xff9aa1a9) : cAccent;

    g.setColour (body);
    g.fillRoundedRectangle (12.5f, 4.0f, 7.0f, 14.0f, 3.5f);
    g.drawLine (16.0f, 22.0f, 16.0f, 27.0f, 2.0f);
    g.drawLine (11.0f, 27.0f, 21.0f, 27.0f, 2.0f);

    juce::Path arc;
    arc.addCentredArc (16.0f, 15.0f, 7.5f, 7.5f, 0.0f,
                       juce::MathConstants<float>::halfPi,
                       juce::MathConstants<float>::halfPi * 3.0f, true);
    g.strokePath (arc, juce::PathStrokeType (2.0f));

    if (muted)
    {
        g.setColour (cDanger);
        g.drawLine (4.0f, 4.0f, 28.0f, 28.0f, 3.5f);
    }

    return image;
}

class TrayIcon final : public juce::SystemTrayIconComponent
{
public:
    std::function<void()> onLeftClick, onRightClick;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick != nullptr) onRightClick();
        }
        else if (onLeftClick != nullptr)
        {
            onLeftClick();
        }
    }
};

//==============================================================================
struct Regions
{
    juce::Rectangle<int> header, devices, waveform, levelRow, voice, record, status;
};

/** paint() と resized() が同じ区画を見るための唯一の計算箇所。
    余白を吸収するのは波形だけ（ui.json のリサイズ方針）。 */
Regions computeRegions (juce::Rectangle<int> area)
{
    Regions r;
    r.header   = area.removeFromTop (60);
    r.status   = area.removeFromBottom (44);
    r.devices  = area.removeFromTop (116);
    r.record   = area.removeFromBottom (72);
    r.voice    = area.removeFromBottom (134);
    r.levelRow = area.removeFromBottom (32);
    r.waveform = area;
    return r;
}

constexpr int kMargin = 20;

} // anonymous namespace

//==============================================================================
MainComponent::MainComponent (AppContext context)
    : ctx (context),
      deviceSelector (context.catalog),
      waveform (context.engine.getInputPeakRing(), context.engine.getInputRawRing()),
      levelMeter (context.engine.getInputPeakRing())
{
    setOpaque (true);

    // 走査結果とクラッシュ記録は設定と同じ場所に置く。起動時に走査はしない
    // （プラグインを 1 つずつ実際に読み込むので数秒かかる）。前回の結果を使う。
    pluginHost = std::make_unique<PluginHost> (
        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("SimpleVoiceChanger"));

    //--------------------------------------------------------------- ヘッダ
    styleLabel (titleLabel, jp ("簡単ボイチェン"), 19.0f, cText, true);
    addAndMakeVisible (titleLabel);

    styleLabel (characterCaption, jp ("いまの声の特徴"), 12.0f, cTextDim);
    addAndMakeVisible (characterCaption);

    // 判定結果。初期値は「ーーー」。一度声を出したら無音でも消さない。
    styleLabel (characterLabel, jp ("ーーー"), 26.0f, cTextDim, true);
    characterLabel.setTooltip (jp ("声の高さ・声道の長さ・かすれ具合・明るさ・抑揚から\n"
                                   "いちばん近い特徴を選んで出しています。\n"
                                   "学習済みAIではなく音響的な目安なので、外れることもあります。\n"
                                   "対象はボイチェン後の声です。"));
    addAndMakeVisible (characterLabel);

    analyser = std::make_unique<VoiceAnalyser> (ctx.engine.getOutputRawRing());

    // 既定 OFF。詳細設定の「声の特徴を自動判別（ベータ）」で有効にする。
    characterVisible = ctx.settings.getCharacterEnabled();
    characterLabel.setVisible (characterVisible);
    characterCaption.setVisible (characterVisible);

    muteButton.setTooltip (jp ("相手への送信だけを止めます。自分の声は聞こえ続けます。"));
    muteButton.onClick = [this] { setMuted (! ctx.params.muted.load (std::memory_order_relaxed)); };
    addAndMakeVisible (muteButton);

    styleLabel (statePill, "", 13.0f, cAccent, true, juce::Justification::centred);
    addAndMakeVisible (statePill);

    styleLabel (hotkeyHintLabel, "", 12.5f, cTextDim, false, juce::Justification::centred);
    hotkeyHintLabel.setInterceptsMouseClicks (true, false);
    hotkeyHintLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    hotkeyHintLabel.setTooltip (jp ("クリックするとキーを変更できます"));
    hotkeyHintLabel.addMouseListener (this, false);
    addAndMakeVisible (hotkeyHintLabel);

    //--------------------------------------------------------------- ① 機器
    deviceSelector.addListener (this);
    addAndMakeVisible (deviceSelector);

    //--------------------------------------------------------------- 波形
    addAndMakeVisible (waveform);
    addAndMakeVisible (levelMeter);

    styleLabel (levelLabel, jp ("入力レベル"), 12.0f, cTextDim);
    addAndMakeVisible (levelLabel);

    styleLabel (clipLabel, jp ("音が大きすぎます"), 12.0f, cDanger);
    clipLabel.setVisible (false);
    addAndMakeVisible (clipLabel);

    //--------------------------------------------------------------- ② 声を変える
    styleLabel (voiceSectionLabel, jp ("② 声を変える"), 15.0f, cText, true);
    addAndMakeVisible (voiceSectionLabel);

    engineCombo.setTooltip (jp ("声を作る仕組みを選びます。内蔵は追加インストール不要ですが、\n"
                                "外部プラグイン（Pitchproof / Graillon / élastique など）の方が自然な音になります。"));
    engineCombo.onChange = [this] { engineSelectionChanged(); };
    addAndMakeVisible (engineCombo);

    pluginRescanButton.setButtonText (jp ("プラグイン検索"));
    pluginRescanButton.setTooltip (jp ("VST3 フォルダを調べ直します。数秒かかることがあります。"));
    pluginRescanButton.onClick = [this] { rescanPlugins(); };
    addAndMakeVisible (pluginRescanButton);

    styleLabel (inputChannelLabel, jp ("入力ch"), 12.0f, cTextDim);
    addAndMakeVisible (inputChannelLabel);

    inputChannelCombo.setTooltip (jp ("マイクが挿さっている入力チャンネルを選びます。\n"
                                      "オーディオインターフェースは入力を何本も持つので、\n"
                                      "ここが合っていないと音が入りません。"));
    inputChannelCombo.onChange = [this]
    {
        const int id = inputChannelCombo.getSelectedId();

        if (id == 0)
            return;

        // id は 1 起点。1 = 先頭2chミックス、2 以降が個別チャンネル。
        ctx.settings.setInputChannelIndex (id == 1 ? -1 : id - 2);
        ctx.settings.scheduleSave();
        restartEngineFromUi();
    };
    addAndMakeVisible (inputChannelCombo);

    optimiseLatencyButton.setButtonText (jp ("遅延を最短に"));
    optimiseLatencyButton.setTooltip (jp ("入力とモニターを 1 台の機器にまとめます。\n"
                                          "2 台にまたがっていると、クロックのずれを吸収する同期バッファが必要になり、\n"
                                          "それだけで 20〜30 ms 増えます。まとめると丸ごと不要になります。\n\n"
                                          "★マイクの機器も変わります。その機器にマイクを繋いでいないと音が入りません。"));
    optimiseLatencyButton.onClick = [this]
    {
        const auto prevInput   = deviceSelector.getInputEndpointId();
        const auto prevMonitor = deviceSelector.getMonitorEndpointId();
        const auto sendId      = deviceSelector.getSendEndpointId();

        const auto rec = ctx.catalog.recommendCollapse (prevInput, prevMonitor);

        const bool alreadyCollapsed = rec.collapsed
                                       && rec.inputId == prevInput && rec.monitorId == prevMonitor;

        if (rec.collapsed && ! alreadyCollapsed)
        {
            deviceSelector.setSelection (rec.inputId, rec.monitorId, sendId);
            deviceSelectionChanged();
        }

        // ★バッファサイズは計算では決められない。ドライバの申告遅延は単調ではなく、
        //   MOTU M Series は 64 の方が 128 より in+out で 160 サンプル短い
        //   （出力側が 203 → 299 に跳ねる）。実際に開いて確かめるしかない。
        juce::String probeNote;
        {
            const auto target = rec.collapsed ? rec.inputId : deviceSelector.getInputEndpointId();

            ctx.engine.stop();
            juce::MouseCursor::showWaitCursor();

            const auto probe = AudioEngine::probeBestBufferSize (
                                   ctx.catalog, target,
                                   ctx.settings.getAllowAsio(), ctx.settings.getAllowExclusive(),
                                   ctx.settings.getRequestedSampleRate() > 0.0
                                       ? ctx.settings.getRequestedSampleRate() : kPreferredSampleRate);

            juce::MouseCursor::hideWaitCursor();

            if (probe.bestBlockSize > 0)
            {
                ctx.settings.setRequestedBlockSize (probe.bestBlockSize);
                ctx.settings.scheduleSave();
                probeNote = probe.summaryJapanese;
            }

            restartEngineFromUi();
        }

        // ★開けなかったら必ず元へ戻す。ここで放置すると無音のまま取り残される
        //   （ASIO は他アプリが掴んでいると開けないので、失敗は普通に起きる）。
        if (! ctx.engine.getStatus().running)
        {
            const auto* entry = ctx.catalog.findInput (rec.inputId);
            const auto name = entry != nullptr ? entry->friendlyName : rec.inputId;

            deviceSelector.setSelection (prevInput, prevMonitor, sendId);
            deviceSelectionChanged();

            showTransient (*this, statusLabel,
                           jp ("「") + name + jp ("」を開けませんでした。ほかのアプリが使用中の可能性があります。"
                                                  "元の設定に戻しました。"), true, 20000);
            return;
        }

        juce::String note = probeNote;

        // 開けても、その機器にマイクが繋がっていなければ無音になる。
        // 「壊れた」と誤解されやすいので、入力が変わったことを明示する。
        if (rec.collapsed && rec.inputId != prevInput)
        {
            const auto* entry = ctx.catalog.findInput (rec.inputId);
            const auto name = entry != nullptr ? entry->friendlyName : rec.inputId;

            note << jp ("マイクも「") + name + jp ("」に切り替えました。"
                        "この機器にマイクを繋いでいない場合は入力を戻してください。");
        }

        if (note.isNotEmpty())
            showTransient (*this, statusLabel, note, false, 25000);
    };
    addAndMakeVisible (optimiseLatencyButton);

    pluginEditorButton.setButtonText (jp ("画面"));
    pluginEditorButton.setTooltip (jp ("プラグイン自身の画面を開きます。"));
    pluginEditorButton.onClick = [this] { openPluginEditor(); };
    addAndMakeVisible (pluginEditorButton);

    static const juce::String chipNames[4] = { jp ("元の声"), jp ("高め（女性寄り）"), jp ("低め（男性寄り）"), jp ("かわいい") };

    for (int i = 0; i < 4; ++i)
    {
        presetChips[i].setButtonText (chipNames[i]);
        presetChips[i].setTooltip (jp ("よく使う設定をまとめて切り替えます"));
        presetChips[i].onClick = [this, i] { applyPreset (static_cast<PresetKind> (i)); };
        addAndMakeVisible (presetChips[i]);
    }

    styleToggle (pitchEnableSwitch, jp ("使う"));
    pitchEnableSwitch.onClick = [this]
    {
        ctx.params.pitchEnabled.store (pitchEnableSwitch.getToggleState(), std::memory_order_relaxed);
        ctx.settings.setPitchEnabled (pitchEnableSwitch.getToggleState());
        refreshVoiceControls();
    };
    addAndMakeVisible (pitchEnableSwitch);

    styleToggle (formantEnableSwitch, jp ("使う"));
    formantEnableSwitch.onClick = [this]
    {
        ctx.params.formantEnabled.store (formantEnableSwitch.getToggleState(), std::memory_order_relaxed);
        ctx.settings.setFormantEnabled (formantEnableSwitch.getToggleState());
        refreshVoiceControls();
    };
    addAndMakeVisible (formantEnableSwitch);

    styleLabel (pitchLabel, jp ("声の高さ（ピッチ）"), 14.0f, cText);
    addAndMakeVisible (pitchLabel);

    styleLabel (formantLabel, jp ("声の太さ（フォルマント）"), 14.0f, cText);
    addAndMakeVisible (formantLabel);

    auto setupSlider = [] (juce::Slider& s, const juce::String& tooltip)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (-12.0, 12.0, 1.0);
        s.setValue (0.0, juce::dontSendNotification);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setDoubleClickReturnValue (true, 0.0);
        s.setTooltip (tooltip);
        s.setColour (juce::Slider::trackColourId, cAccent.withAlpha (0.65f));
        s.setColour (juce::Slider::backgroundColourId, cPanelHi);
        s.setColour (juce::Slider::thumbColourId, cAccent);
    };

    setupSlider (pitchSlider,   jp ("低く ←→ 高く。数字は半音の数です。"));
    setupSlider (formantSlider, jp ("声の響きの太さを変えます。ピッチと一緒に動かすと自然に聞こえます。"));

    pitchSlider.onValueChange = [this]
    {
        const auto v = static_cast<float> (pitchSlider.getValue());
        ctx.params.pitchSemitones.store (v, std::memory_order_relaxed);
        ctx.settings.setPitchSemitones (v);
        refreshVoiceControls();
    };

    formantSlider.onValueChange = [this]
    {
        const auto v = static_cast<float> (formantSlider.getValue());
        ctx.params.formantSemitones.store (v, std::memory_order_relaxed);
        ctx.settings.setFormantSemitones (v);
        refreshVoiceControls();
    };

    addAndMakeVisible (pitchSlider);
    addAndMakeVisible (formantSlider);

    styleLabel (pitchValueLabel,   jp ("0 半音"), 16.0f, cText, true, juce::Justification::centred);
    styleLabel (formantValueLabel, jp ("0 半音"), 16.0f, cText, true, juce::Justification::centred);
    addAndMakeVisible (pitchValueLabel);
    addAndMakeVisible (formantValueLabel);

    pitchResetButton.setButtonText (jp ("0に戻す"));
    pitchResetButton.onClick = [this] { pitchSlider.setValue (0.0, juce::sendNotificationSync); };
    addAndMakeVisible (pitchResetButton);

    formantResetButton.setButtonText (jp ("0に戻す"));
    formantResetButton.onClick = [this] { formantSlider.setValue (0.0, juce::sendNotificationSync); };
    addAndMakeVisible (formantResetButton);

    //--------------------------------------------------------------- ③ 録音
    styleLabel (recordSectionLabel, jp ("③ 録音して確かめる"), 15.0f, cText, true);
    addAndMakeVisible (recordSectionLabel);

    styleLabel (recordHintLabel, jp ("録音し直すと前の録音は消えます。ピコ音とミュートは録音に入りません。"),
                12.5f, cTextDim);
    addAndMakeVisible (recordHintLabel);

    recordButton.setButtonText (jp ("● 録音"));
    recordButton.onClick = [this] { startRecording(); };
    addAndMakeVisible (recordButton);

    playButton.setButtonText (jp ("▶ 再生"));
    playButton.setTooltip (jp ("録音した声を自分のヘッドホンだけで確認します（相手には送られません）"));
    playButton.onClick = [this] { startPlayback(); };
    addAndMakeVisible (playButton);

    stopButton.setButtonText (jp ("■ 停止"));
    stopButton.onClick = [this] { stopRecordingOrPlayback(); };
    addAndMakeVisible (stopButton);

    styleLabel (timeLabel, "00:00", 16.0f, cText, true);
    addAndMakeVisible (timeLabel);

    exportButton.setButtonText (jp ("書き出し（MP3）"));
    exportButton.onClick = [this] { beginExport(); };
    addAndMakeVisible (exportButton);

    //--------------------------------------------------------------- ステータスバー
    styleToggle (noiseSwitch, jp ("ノイズ除去"));
    noiseSwitch.onClick = [this]
    {
        const bool on = noiseSwitch.getToggleState();
        ctx.params.noiseSuppressionEnabled.store (on, std::memory_order_relaxed);
        ctx.settings.setNoiseSuppressionEnabled (on);
        ctx.settings.scheduleSave();

        // OFF のときはノイズ除去を信号経路ごと外して 5.3 ms 削る。
        // 経路の変更は遅延が変わるのでグラフを作り直す（音が一瞬途切れる）。
        restartEngineFromUi();
    };
    addAndMakeVisible (noiseSwitch);

    styleLabel (noiseSubLabel, jp ("エアコンや PC のファンの音を消します"), 12.5f, cTextDim);
    addAndMakeVisible (noiseSubLabel);

    styleLabel (statusLabel, jp ("準備しています…"), 12.0f, cTextDim);
    addAndMakeVisible (statusLabel);

    advancedButton.setButtonText (jp ("詳細設定"));
    advancedButton.onClick = [this] { openAdvancedSettings(); };
    addAndMakeVisible (advancedButton);

    // 色付け（レイアウトとは独立なのでまとめて）
    styleButton (recordButton,      cPanelHi, cText);
    styleButton (playButton,        cPanelHi, cText);
    styleButton (stopButton,        cPanelHi, cText);
    styleButton (exportButton,      cAccent,  juce::Colour (0xff10151a));
    styleButton (advancedButton,    cPanel.brighter (0.08f), cTextDim);
    styleButton (pitchResetButton,  cPanelHi, cTextDim);
    styleButton (formantResetButton, cPanelHi, cTextDim);

    for (auto& chip : presetChips)
        styleButton (chip, cPanelHi, cText);

    //--------------------------------------------------------------- トレイ
    {
        auto icon = std::make_unique<TrayIcon>();
        juce::Component::SafePointer<MainComponent> safe (this);

        icon->onLeftClick = [safe]
        {
            if (safe == nullptr) return;

            if (auto* window = safe->getTopLevelComponent())
            {
                window->setVisible (true);

                if (auto* doc = dynamic_cast<juce::ResizableWindow*> (window))
                    doc->setMinimised (false);

                window->toFront (true);
            }
        };

        icon->onRightClick = [safe]
        {
            if (safe == nullptr) return;

            const bool muted = safe->ctx.params.muted.load (std::memory_order_relaxed);

            juce::PopupMenu menu;
            menu.addItem (1, muted ? jp ("ミュートを解除") : jp ("ミュートする"));
            menu.addItem (2, jp ("ウィンドウを表示"));
            menu.addSeparator();
            menu.addItem (3, jp ("終了"));

            menu.showMenuAsync (juce::PopupMenu::Options(), [safe] (int result)
            {
                if (safe == nullptr) return;

                if (result == 1)
                {
                    safe->setMuted (! safe->ctx.params.muted.load (std::memory_order_relaxed));
                }
                else if (result == 2)
                {
                    if (auto* window = safe->getTopLevelComponent())
                    {
                        window->setVisible (true);
                        window->toFront (true);
                    }
                }
                else if (result == 3)
                {
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
                }
            });
        };

        trayIcon = std::move (icon);
    }

    ctx.engine.addListener (this);
    ctx.hotkey.addListener (this);

    setSize (kDefaultWidth, kDefaultHeight);
    // 声質判定は 20 Hz で回す。それ以外の表示更新は 5 回に 1 回（従来どおり 4 Hz）。
    startTimer (VoiceAnalyser::kTickIntervalMs);
}

MainComponent::~MainComponent()
{
    stopTimer();

    ctx.hotkey.removeListener (this);
    ctx.engine.removeListener (this);
    deviceSelector.removeListener (this);

    fileChooser.reset();
    trayIcon.reset();
}

//==============================================================================
void MainComponent::applySettingsAndStartEngine()
{
    auto& settings = ctx.settings;

    juce::String inputId   = settings.getInputEndpointId();
    juce::String monitorId = settings.getMonitorEndpointId();
    juce::String sendId    = settings.getSendEndpointId();

    if (settings.isFirstRun())
    {
        // 入力とモニターが同一デバイスになる構成を最優先で選ぶ。ドリフト補正リングが
        // 不要になり、別デバイス構成より 20〜30 ms 速い。これが遅延の最大の効きどころ。
        const auto rec = ctx.catalog.recommendInitialDevices();

        inputId   = rec.inputId;
        monitorId = rec.monitorId;

        // 初回は「相手に送る音」をわざと未選択にして、ステータスバーで選ばせる。
        sendId.clear();
    }

    deviceSelector.setAdvancedBackendsVisible (settings.getAllowAsio() || settings.getAllowExclusive());
    deviceSelector.setSelection (inputId, monitorId, sendId);

    settings.setInputEndpointId (deviceSelector.getInputEndpointId());
    settings.setMonitorEndpointId (deviceSelector.getMonitorEndpointId());
    settings.setSendEndpointId (deviceSelector.getSendEndpointId());
    settings.scheduleSave();

    const auto binding = settings.getHotkeyBinding();
    const auto installResult = ctx.hotkey.install (binding);

    auto& beep = ctx.engine.getBeepGenerator();
    beep.setEnabled (ctx.params.beepEnabled.load (std::memory_order_relaxed));
    beep.setVolume  (ctx.params.beepVolume.load  (std::memory_order_relaxed));

    noiseSwitch.setToggleState (ctx.params.noiseSuppressionEnabled.load (std::memory_order_relaxed),
                                juce::dontSendNotification);

    exportButton.setButtonText (settings.getExportFormat() == Mp3Exporter::Format::wav
                                  ? jp ("書き出し（WAV）") : jp ("書き出し（MP3）"));

    refreshVoiceControls();
    refreshMuteUi();

    // エンジンより先にプラグインを用意する。GraphRequest がその生ポインタを持つため。
    loadPluginFromSettings();
    rebuildEngineCombo();

    restartEngineFromUi();

    // ホットキーの失敗はエンジンの起動メッセージより後に出す（こちらの方が寿命が長い）。
    if (installResult != GlobalHotkey::InstallResult::ok)
        showTransient (*this, statusLabel, GlobalHotkey::japaneseError (installResult), true, 20000);
}

void MainComponent::prepareToShutDown()
{
    stopTimer();

    if (ctx.recorder.isRecording())
        ctx.recorder.stopRecording();

    ctx.engine.stopTakePlayback();

    ctx.hotkey.removeListener (this);
    ctx.engine.removeListener (this);
    deviceSelector.removeListener (this);

    ctx.engine.stop();

    // プラグインの設定を保存してから、エディタ → 本体 の順に片付ける。
    // 逆にするとエディタが死んだプラグインを触って落ちる。
    if (plugin != nullptr)
    {
        ctx.settings.setPluginState (plugin->saveState());
        ctx.settings.scheduleSave();
    }

    closePluginEditor();
    plugin.reset();

    trayIcon.reset();
    fileChooser.reset();

    ctx.settings.setInputEndpointId (deviceSelector.getInputEndpointId());
    ctx.settings.setMonitorEndpointId (deviceSelector.getMonitorEndpointId());
    ctx.settings.setSendEndpointId (deviceSelector.getSendEndpointId());
    ctx.settings.captureFromParams (ctx.params);
    ctx.settings.saveIfNeeded();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (cBackground);

    const auto r = computeRegions (getLocalBounds());

    g.setColour (cPanel);
    g.fillRect (r.header);

    g.setColour (cBackground.brighter (0.06f));
    g.fillRect (r.header.getX(), r.header.getBottom() - 1, r.header.getWidth(), 1);

    auto panel = [&g] (juce::Rectangle<int> area)
    {
        g.setColour (cPanel);
        g.fillRoundedRectangle (area.reduced (8, 2).toFloat(), 8.0f);
    };

    panel (r.devices);
    panel (r.voice);
    panel (r.record);

    g.setColour (cPanel);
    g.fillRect (r.status);
    g.setColour (cBackground.brighter (0.06f));
    g.fillRect (r.status.getX(), r.status.getY(), r.status.getWidth(), 1);

    // ミュートしていない間だけ、巨大ミュートボタンを緑で縁取る。
    if (! ctx.params.muted.load (std::memory_order_relaxed))
    {
        g.setColour (cAccent.withAlpha (0.85f));
        g.drawRoundedRectangle (muteButton.getBounds().toFloat().expanded (1.5f), 7.0f, 2.0f);
    }
}

void MainComponent::resized()
{
    const auto r = computeRegions (getLocalBounds());

    //--------------------------------------------------------------- ヘッダ
    {
        auto area = r.header.reduced (kMargin, 0);

        titleLabel.setBounds (area.getX(), area.getY() + 8, 200, 24);
        characterCaption.setBounds (area.getX(), area.getY() + 32, 200, 14);

        const int pillWidth = 112;
        statePill.setBounds (area.getRight() - pillWidth, area.getY() + 16, pillWidth, 28);

        const int muteWidth = 200;
        const int muteX = statePill.getX() - 14 - muteWidth;
        muteButton.setBounds (muteX, area.getY() + 4, muteWidth, 40);
        hotkeyHintLabel.setBounds (muteX, area.getY() + 44, muteWidth, 14);

        // 声質判定はタイトルとミュートボタンの間。ここが空いていて最も目に入る。
        const int charX = titleLabel.getRight() + 16;
        characterLabel.setBounds (charX, area.getY() + 8, juce::jmax (120, muteX - 16 - charX), 38);
    }

    //--------------------------------------------------------------- ① 機器
    deviceSelector.setBounds (r.devices.reduced (kMargin, 3));

    //--------------------------------------------------------------- 波形
    waveform.setBounds (r.waveform.reduced (kMargin, 6));

    {
        auto area = r.levelRow.reduced (kMargin, 0);

        levelLabel.setBounds (area.getX(), area.getY() + 5, 76, 20);

        // 右側に「入力ch」と「遅延を最短に」を置くぶんメーターを詰める。
        const int meterWidth = juce::jlimit (140, 320, area.getWidth() - 80 - 130 - 380);
        levelMeter.setBounds (area.getX() + 80, area.getY() + 8, meterWidth, 14);

        clipLabel.setBounds (levelMeter.getRight() + 14, area.getY() + 5, 120, 20);

        optimiseLatencyButton.setBounds (area.getRight() - 150, area.getY() + 4, 150, 22);
        inputChannelCombo.setBounds (optimiseLatencyButton.getX() - 6 - 150, area.getY() + 4, 150, 22);
        inputChannelLabel.setBounds (inputChannelCombo.getX() - 6 - 62, area.getY() + 5, 62, 20);
    }

    //--------------------------------------------------------------- ② 声を変える
    {
        auto area = r.voice.reduced (kMargin, 0);
        int y = area.getY() + 6;

        voiceSectionLabel.setBounds (area.getX(), y, 150, 22);

        // ② の見出し行の右側に音声処理エンジンの選択を置く。
        // プラグインが本命の利用者もいるので、詳細設定に埋めずに主画面へ出す。
        {
            const int editW = 86, rescanW = 96, gap = 6;
            const int right = area.getRight();

            pluginEditorButton.setBounds (right - editW, y, editW, 22);
            pluginRescanButton.setBounds (right - editW - gap - rescanW, y, rescanW, 22);

            const int comboX = area.getX() + 156;
            const int comboW = juce::jmax (120, pluginRescanButton.getX() - gap - comboX);
            engineCombo.setBounds (comboX, y, comboW, 22);
        }

        y += 26;

        const int chipWidth = 132, chipHeight = 26, chipGap = 8;
        for (int i = 0; i < 4; ++i)
            presetChips[i].setBounds (area.getX() + i * (chipWidth + chipGap), y, chipWidth, chipHeight);

        y += chipHeight + 8;

        auto layoutRow = [&area] (int rowY,
                                  juce::ToggleButton& toggle, juce::Label& name,
                                  juce::Slider& slider, juce::Label& value, juce::TextButton& reset)
        {
            const int left  = area.getX();
            const int right = area.getRight();

            toggle.setBounds (left, rowY + 5, 60, 20);
            name.setBounds   (left + 66, rowY + 4, 150, 22);

            reset.setBounds (right - 84, rowY + 3, 84, 24);
            value.setBounds (reset.getX() - 86, rowY + 3, 78, 24);
            slider.setBounds (left + 222, rowY, value.getX() - (left + 222) - 10, 30);
        };

        layoutRow (y, pitchEnableSwitch, pitchLabel, pitchSlider, pitchValueLabel, pitchResetButton);
        y += 34;
        layoutRow (y, formantEnableSwitch, formantLabel, formantSlider, formantValueLabel, formantResetButton);
    }

    //--------------------------------------------------------------- ③ 録音
    {
        auto area = r.record.reduced (kMargin, 0);

        recordSectionLabel.setBounds (area.getX(), area.getY() + 4, 220, 20);
        recordHintLabel.setBounds (area.getX() + 226, area.getY() + 6, area.getWidth() - 226, 18);

        const int y = area.getY() + 30;
        recordButton.setBounds (area.getX(), y, 132, 36);
        playButton.setBounds   (area.getX() + 140, y, 112, 36);
        stopButton.setBounds   (area.getX() + 260, y, 112, 36);
        timeLabel.setBounds    (area.getX() + 386, y, 160, 36);
        exportButton.setBounds (area.getRight() - 170, y, 170, 36);
    }

    //--------------------------------------------------------------- ステータスバー
    {
        auto area = r.status.reduced (kMargin, 0);

        noiseSwitch.setBounds (area.getX(), area.getY() + 4, 130, 20);
        noiseSubLabel.setBounds (area.getX(), area.getY() + 23, 240, 16);

        advancedButton.setBounds (area.getRight() - 96, area.getY() + 8, 96, 28);

        const int statusX = area.getX() + 254;
        statusLabel.setBounds (statusX, area.getY() + 12,
                               juce::jmax (100, advancedButton.getX() - statusX - 12), 20);
    }
}

//==============================================================================
void MainComponent::deviceSelectionChanged()
{
    ctx.settings.setInputEndpointId (deviceSelector.getInputEndpointId());
    ctx.settings.setMonitorEndpointId (deviceSelector.getMonitorEndpointId());
    ctx.settings.setSendEndpointId (deviceSelector.getSendEndpointId());
    ctx.settings.scheduleSave();

    restartEngineFromUi();
}

void MainComponent::deviceRescanRequested()
{
    ctx.catalog.rescan();
    deviceSelector.refresh();
    showTransient (*this, statusLabel, jp ("機器を再検索しました"), false, 3000);
}

//==============================================================================
void MainComponent::engineStatusChanged()
{
    const auto status = ctx.engine.getStatus();
    updateDeviceStatusLines (deviceSelector, status, deviceSelector.getSendEndpointId().isNotEmpty());
    refreshLatencyDisplay();

    // チャンネル名はデバイスを開いて初めて分かるので、ここで作り直す。
    rebuildInputChannelCombo();

    repaint();
}

void MainComponent::engineErrorOccurred (const juce::String& japaneseMessage)
{
    showTransient (*this, statusLabel, japaneseMessage, true, 20000);
}

void MainComponent::engineDeviceListChanged()
{
    deviceSelector.refresh();
    showTransient (*this, statusLabel, jp ("機器の構成が変わりました。選択を確認してください。"), true, 12000);
    engineStatusChanged();
}

//==============================================================================
void MainComponent::hotkeyPressed()
{
    if (ctx.hotkey.getBinding().mode == HotkeyMode::pushToMute)
        setMuted (true);
    else
        setMuted (! ctx.params.muted.load (std::memory_order_relaxed));
}

void MainComponent::hotkeyReleased()
{
    if (ctx.hotkey.getBinding().mode == HotkeyMode::pushToMute)
        setMuted (false);
}

void MainComponent::hotkeyBackendRecovered()
{
    showTransient (*this, statusLabel,
                   jp ("⚠ ショートカットキーが効かなくなりました。再設定しました。"), true, 12000);
}

void MainComponent::mouseUp (const juce::MouseEvent& e)
{
    if (e.eventComponent == &hotkeyHintLabel)
        openHotkeyDialog();
}

//==============================================================================
void MainComponent::timerCallback()
{
    // 声質判定だけ毎回（20 Hz）。RawRing を読むだけでオーディオ側には一切触れない。
    // 既定 OFF の任意機能なので、無効な間は表示ごと隠して計算も一切しない。
    {
        const bool wanted = ctx.settings.getCharacterEnabled();

        if (wanted != characterVisible)
        {
            characterVisible = wanted;
            characterLabel.setVisible (wanted);
            characterCaption.setVisible (wanted);

            // 有効化したときに前回の古い判定が残っていると誤解を生む。
            if (analyser != nullptr)
                analyser->reset();

            refreshCharacterLabel();
        }

        if (wanted && analyser != nullptr)
        {
            const double interval = ctx.settings.getCharacterIntervalMs() / 1000.0;

            if (analyser->tick (interval))
                refreshCharacterLabel();
        }
    }

    // 以降は 5 回に 1 回＝従来どおりの 4 Hz。
    if (++analyserTickDivider < 5)
        return;

    analyserTickDivider = 0;

    // ハウリング保護。作動している間は出し続ける（一度きりの通知だと見逃す）。
    if (ctx.engine.isMonitorProtectionEngaged())
    {
        if (! howlWarningShown)
        {
            howlWarningShown = true;

            showTransient (*this, statusLabel,
                           jp ("⚠ ハウリングを検出したため、自分に聞こえる音を止めました。"
                               "マイクがスピーカーの音を拾っています。ヘッドホンを使うか、"
                               "「自分に聞こえる音」を別の機器にしてください。"), true, 60000);
        }
    }
    else
    {
        howlWarningShown = false;
    }

    const bool recording = ctx.recorder.isRecording();
    const bool playing   = ctx.engine.isPlayingTake();
    const bool hasTake   = ctx.recorder.hasTake();

    //--- 経過時間
    if (recording)
    {
        timeLabel.setText (formatMmSs (ctx.recorder.getRecordedSeconds()), juce::dontSendNotification);
    }
    else if (playing)
    {
        timeLabel.setText (formatMmSs (ctx.engine.getPlaybackPositionSeconds()) + " / "
                            + formatMmSs (ctx.engine.getPlaybackLengthSeconds()),
                           juce::dontSendNotification);
    }
    else
    {
        timeLabel.setText (formatMmSs (ctx.recorder.getRecordedSeconds()), juce::dontSendNotification);
    }

    //--- ボタンの活性
    recordButton.setButtonText (recording ? jp ("● 録音中") : jp ("● 録音"));
    recordButton.setEnabled (! recording);
    playButton.setEnabled (hasTake && ! recording && ! playing);
    stopButton.setEnabled (recording || playing);
    exportButton.setEnabled (hasTake && ! recording && ! exportInProgress);

    if (recording)
    {
        const bool blink = ((juce::Time::getMillisecondCounter() / 500) % 2) == 0;
        styleButton (recordButton, blink ? cDanger : cDanger.darker (0.45f), cText);
        recordButton.repaint();
    }
    else
    {
        styleButton (recordButton, cPanelHi, cText);
    }

    //--- クリップ表示
    clipLabel.setVisible (levelMeter.isClippingRecently());

    //--- 録音の取りこぼし
    if (ctx.recorder.didOverrun())
    {
        ctx.recorder.clearOverrunFlag();
        showTransient (*this, statusLabel, jp ("録音が追いつきませんでした"), true, 8000);
    }

    //--- ドリフトリングのアンダーラン（増えたときだけ出す）
    {
        const auto send = ctx.engine.getSendDiagnostics();
        const auto mon  = ctx.engine.getMonitorDiagnostics();
        const int total = send.underruns + mon.underruns;
        const int known = static_cast<int> (getProperties().getWithDefault (kXrunKey, juce::var (0)));

        if (total > known)
        {
            getProperties().set (kXrunKey, total);

            if (known > 0 || total > 2)
                showTransient (*this, statusLabel,
                               jp ("音が途切れました（詳細設定でバッファサイズを大きくしてください）"),
                               true, 8000);
        }
    }

    //--- ステータス文言（一時メッセージが生きている間は触らない）
    if (! transientStillShowing (*this))
    {
        if (exportInProgress)
        {
            setStatusText (statusLabel, jp ("書き出しています…"), false);
        }
        else if (recording)
        {
            setStatusText (statusLabel, jp ("録音中…（相手に聞こえるのと同じ音を録っています）"), false);
        }
        else if (playing)
        {
            setStatusText (statusLabel, jp ("再生中…（この音は相手には送られません）"), false);
        }
        else if (! ctx.engine.isRunning())
        {
            const auto error = ctx.engine.getStatus().lastErrorJapanese;
            setStatusText (statusLabel, error.isNotEmpty() ? error : juce::String (jp ("停止中")), true);
        }
        else if (deviceSelector.getSendEndpointId().isEmpty())
        {
            setStatusText (statusLabel,
                           jp ("「相手に送る音」を選んでください（SYNCROOM なら Yamaha SYNCROOM Driver）"),
                           false);
        }
        else if (ctx.engine.isSilentInputSuspected())
        {
            setStatusText (statusLabel,
                           jp ("マイクへのアクセスが許可されていない可能性があります（設定 → プライバシー → マイク）"),
                           true);
        }
        else
        {
            // SYNCROOM に送っているあいだは、相手側の設定手順を出し続ける。
            // ここを一度でも見逃すと「音が届かない」で詰まる箇所なので、常時表示にしている。
            const auto* sendEntry = ctx.catalog.findOutput (deviceSelector.getSendEndpointId());

            if (sendEntry != nullptr && sendEntry->friendlyName.containsIgnoreCase ("SYNCROOM"))
                setStatusText (statusLabel,
                               jp ("準備完了 ／ SYNCROOM 側は「設定 → オーディオ → WASAPI共有モード」にして、"
                               "入力にこのドライバを選んでください"),
                               false);
            else
                setStatusText (statusLabel, jp ("準備完了"), false);
        }
    }
}

//==============================================================================
void MainComponent::setMuted (bool shouldBeMuted)
{
    ctx.params.muted.store (shouldBeMuted, std::memory_order_relaxed);

    // ビープはモニターバスにしか足されない（送信側にはコードが存在しない）。
    ctx.engine.getBeepGenerator().trigger (shouldBeMuted ? BeepGenerator::Kind::muteOn
                                                         : BeepGenerator::Kind::muteOff);
    refreshMuteUi();
}

void MainComponent::refreshMuteUi()
{
    const bool muted = ctx.params.muted.load (std::memory_order_relaxed);
    const auto keyName = GlobalHotkey::describeBinding (ctx.settings.getHotkeyBinding());

    muteButton.setButtonText (muted ? jp ("ミュート中") : jp ("ミュート"));
    styleButton (muteButton, muted ? cDanger : cPanel.brighter (0.10f),
                             muted ? juce::Colour (0xff14171a) : cAccent);

    hotkeyHintLabel.setText (muted ? (keyName + jp (" で解除")) : (keyName + jp (" キー")),
                             juce::dontSendNotification);

    statePill.setText (muted ? jp ("● ミュート中") : jp ("● 音声オン"), juce::dontSendNotification);
    statePill.setColour (juce::Label::textColourId, muted ? cDanger : cAccent);

    waveform.setMuted (muted, keyName);

    if (trayIcon != nullptr)
    {
        // ウィンドウを隠していてもホットキーは効く。トレイの見た目が唯一の手掛かりになる。
        const auto image = makeTrayIcon (muted);
        trayIcon->setIconImage (image, image);
        trayIcon->setIconTooltip (muted ? jp ("簡単ボイチェン - ミュート中") : jp ("簡単ボイチェン"));
    }

    repaint();
}

void MainComponent::refreshLatencyDisplay()
{
    const auto status = ctx.engine.getStatus();

    // 「1 台にまとめれば短くなる」構成のときだけボタンを出す。既に最短なら消す。
    {
        const auto currentInput   = deviceSelector.getInputEndpointId();
        const auto currentMonitor = deviceSelector.getMonitorEndpointId();

        // まとめ済みでも、バッファの実測探索がまだならボタンは出しておく
        // （バッファは計算で決められないので、押してもらう価値が常にある）。
        const auto rec = ctx.catalog.recommendCollapse (currentInput, currentMonitor);

        const bool alreadyOptimal = currentInput == rec.inputId
                                     && currentMonitor == rec.monitorId
                                     && ctx.settings.getRequestedBlockSize() > 0;

        optimiseLatencyButton.setVisible (! alreadyOptimal);
    }

    if (! status.running)
    {
        waveform.setLatencyText (jp ("遅延 ―"), false);
        waveform.setTooltip ({});
        return;
    }

    if (status.input.sampleRate > 0.0)
        waveform.setSampleRate (status.input.sampleRate);

    // 「遅延ゼロ」は原理的に不可能。エフェクトを切っても PSOLA の 512 サンプルは
    // 消えない設計なので、正直な合計をそのまま出す（要件 4）。
    const double ms = status.monitorTotalLatencyMs;
    waveform.setLatencyText (jp ("遅延 約 ") + juce::String (ms, 1) + " ms", ms > 25.0);

    // 合計だけ出しても「どこが重いのか」が分からず手の打ちようがない。内訳を出す。
    const double rate = status.input.sampleRate > 0.0 ? status.input.sampleRate : 48000.0;
    const auto toMs = [rate] (int samples) { return juce::String (1000.0 * samples / rate, 1) + " ms"; };

    juce::String t;
    t << jp ("遅延の内訳") << "\n";
    t << jp ("  入力バッファ  ") << status.input.blockSize << " smp / " << toMs (status.input.blockSize) << "\n";
    t << jp ("  音声処理      ") << status.dspLatencySamples << " smp / " << toMs (status.dspLatencySamples) << "\n";

    if (status.collapsed)
    {
        t << jp ("  同期バッファ  なし（入力とモニターが同じ機器）") << "\n";
    }
    else if (ctx.engine.hasMonitorRing())
    {
        const auto d = ctx.engine.getMonitorDiagnostics();
        t << jp ("  同期バッファ  ") << d.targetFill << " smp / " << toMs (d.targetFill)
          << jp ("  ← 入力とモニターを同じ機器にすると消えます") << "\n";
    }

    t << jp ("  出力バッファ  ") << status.monitor.blockSize << " smp / " << toMs (status.monitor.blockSize) << "\n";
    t << jp ("合計 ") << juce::String (ms, 1) << " ms";

    if (status.latencyIsEstimate)
        t << jp ("（推定値を含む）");

    waveform.setTooltip (t);
}

void MainComponent::refreshVoiceControls()
{
    const float pitch   = ctx.params.pitchSemitones.load   (std::memory_order_relaxed);
    const float formant = ctx.params.formantSemitones.load (std::memory_order_relaxed);
    const bool  pitchOn   = ctx.params.pitchEnabled.load   (std::memory_order_relaxed);
    const bool  formantOn = ctx.params.formantEnabled.load (std::memory_order_relaxed);

    pitchSlider.setValue   (pitch,   juce::dontSendNotification);
    formantSlider.setValue (formant, juce::dontSendNotification);

    pitchEnableSwitch.setToggleState   (pitchOn,   juce::dontSendNotification);
    formantEnableSwitch.setToggleState (formantOn, juce::dontSendNotification);

    pitchValueLabel.setText   (signedSemitones (pitch),   juce::dontSendNotification);
    formantValueLabel.setText (signedSemitones (formant), juce::dontSendNotification);

    auto setRowActive = [] (bool on, juce::Slider& s, juce::Label& value, juce::TextButton& reset)
    {
        // ラベルはそのまま。行が読めなくならないように、操作部だけ薄くする。
        s.setEnabled (on);      s.setAlpha (on ? 1.0f : 0.4f);
        value.setAlpha (on ? 1.0f : 0.4f);
        reset.setEnabled (on);  reset.setAlpha (on ? 1.0f : 0.4f);
    };

    setRowActive (pitchOn,   pitchSlider,   pitchValueLabel,   pitchResetButton);
    setRowActive (formantOn, formantSlider, formantValueLabel, formantResetButton);

    for (int i = 0; i < 4; ++i)
    {
        const auto preset = presetValues (static_cast<PresetKind> (i));
        const bool active = preset.pitchEnabled == pitchOn
                         && preset.formantEnabled == formantOn
                         && std::fabs (preset.pitchSemitones   - pitch)   < 0.01f
                         && std::fabs (preset.formantSemitones - formant) < 0.01f;

        styleButton (presetChips[i], active ? cAccent.withAlpha (0.28f) : cPanelHi,
                                     active ? cAccent : cText);
        presetChips[i].repaint();
    }

    ctx.settings.scheduleSave();
}

void MainComponent::applyPreset (PresetKind kind)
{
    ctx.params.applyPreset (kind);

    const auto values = presetValues (kind);
    ctx.settings.setPitchSemitones (values.pitchSemitones);
    ctx.settings.setFormantSemitones (values.formantSemitones);
    ctx.settings.setPitchEnabled (values.pitchEnabled);
    ctx.settings.setFormantEnabled (values.formantEnabled);

    refreshVoiceControls();
}

void MainComponent::restartEngineFromUi()
{
    if (ctx.recorder.isRecording())
        ctx.recorder.stopRecording();

    ctx.engine.stopTakePlayback();
    ctx.engine.stop();

    const auto inputId   = deviceSelector.getInputEndpointId();
    const auto monitorId = deviceSelector.getMonitorEndpointId();
    const auto sendId    = deviceSelector.getSendEndpointId();

    const auto validation = ctx.catalog.validateRouting (inputId, monitorId, sendId);

    if (validation.blocksStart)
    {
        showTransient (*this, statusLabel, validation.japaneseMessage, true, 30000);
        engineStatusChanged();
        return;
    }

    AudioEngine::GraphRequest request;
    request.inputEndpointId   = inputId;
    request.monitorEndpointId = monitorId;
    request.sendEndpointId    = sendId;
    request.allowAsio         = ctx.settings.getAllowAsio();
    request.allowExclusive    = ctx.settings.getAllowExclusive();

    const int blockSize = ctx.settings.getRequestedBlockSize();
    request.requestedBlockSize = blockSize > 0 ? blockSize : 128;

    const double sampleRate = ctx.settings.getRequestedSampleRate();
    request.requestedSampleRate = sampleRate > 0.0 ? sampleRate : kPreferredSampleRate;

    request.fillMode     = ctx.settings.getFillMode();
    request.noiseQuality = ctx.settings.getNoiseQuality();

    // 外部プラグイン。engine.stop() 済みなので、ここで差し替えても実行中の
    // 遅延が動くことはない。plugin の寿命は MainComponent が握っている。
    request.external = plugin.get();

    // ノイズ除去が OFF なら経路ごと外す。STFT の N ぶん（既定 5.3 ms）が消える。
    request.noiseSuppressionInPath = ctx.params.noiseSuppressionEnabled.load (std::memory_order_relaxed);
    request.inputChannelIndex      = ctx.settings.getInputChannelIndex();

    const bool started = ctx.engine.start (request);
    const auto status = ctx.engine.getStatus();

    if (! started)
    {
        showTransient (*this, statusLabel,
                       status.lastErrorJapanese.isNotEmpty() ? status.lastErrorJapanese
                                                             : juce::String (jp ("機器を開けませんでした")),
                       true, 30000);
    }
    else
    {
        // 実際に開けたレートで録音を用意する。要求値ではなく読み戻した値を使う。
        const double actualRate = status.input.sampleRate > 0.0 ? status.input.sampleRate
                                                                : kPreferredSampleRate;
        ctx.recorder.prepare (actualRate, kInternalNumChannels);

        if (validation.problem != RoutingProblem::none && validation.japaneseMessage.isNotEmpty())
            showTransient (*this, statusLabel, validation.japaneseMessage, true, 15000);
    }

    engineStatusChanged();
}

//==============================================================================
void MainComponent::startRecording()
{
    if (ctx.recorder.isRecording())
        return;

    if (! ctx.engine.isRunning())
    {
        showTransient (*this, statusLabel, jp ("先に機器を選んでください（音が流れていません）"), true, 8000);
        return;
    }

    ctx.engine.stopTakePlayback();

    if (! ctx.recorder.startRecording())
    {
        showTransient (*this, statusLabel, jp ("録音を開始できませんでした"), true, 8000);
        return;
    }

    getProperties().set (kStickyKey, juce::int64 (0));
    timerCallback();
}

void MainComponent::stopRecordingOrPlayback()
{
    if (ctx.recorder.isRecording())
    {
        ctx.recorder.stopRecording();
        showTransient (*this, statusLabel, jp ("録音しました。「▶ 再生」で確認できます。"), false, 6000);
    }
    else if (ctx.engine.isPlayingTake())
    {
        ctx.engine.stopTakePlayback();
    }

    timerCallback();
}

void MainComponent::startPlayback()
{
    if (! ctx.recorder.hasTake())
        return;

    if (ctx.recorder.isRecording())
        ctx.recorder.stopRecording();

    if (! ctx.engine.startTakePlayback (ctx.recorder.getTakeFile()))
        showTransient (*this, statusLabel, jp ("録音した音を再生できませんでした"), true, 8000);
}

void MainComponent::beginExport()
{
    if (exportInProgress || ! ctx.recorder.hasTake())
        return;

    if (ctx.recorder.isRecording())
        ctx.recorder.stopRecording();

    auto format = ctx.settings.getExportFormat();

    if (format == Mp3Exporter::Format::mp3 && ! Mp3Exporter::probeMp3Available())
    {
        format = Mp3Exporter::Format::wav;
        showTransient (*this, statusLabel,
                       jp ("この環境には MP3 エンコーダがないため WAV で保存します"), true, 10000);
    }

    auto folder = ctx.settings.getExportFolder();

    if (! folder.isDirectory())
        folder = Mp3Exporter::getDefaultExportFolder();

    const auto wildcard = format == Mp3Exporter::Format::mp3 ? juce::String ("*.mp3")
                                                             : juce::String ("*.wav");

    fileChooser = std::make_unique<juce::FileChooser> (
        jp ("保存先を選んでください"),
        folder.getChildFile (Mp3Exporter::makeDefaultFileName (format)),
        wildcard);

    juce::Component::SafePointer<MainComponent> safe (this);

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, format] (const juce::FileChooser& chooser)
    {
        if (safe == nullptr)
            return;

        const auto destination = chooser.getResult();

        if (destination == juce::File())
            return;

        safe->ctx.settings.setExportFolder (destination.getParentDirectory());
        safe->ctx.settings.scheduleSave();

        Mp3Exporter::Request request;
        request.sourceWav   = safe->ctx.recorder.getTakeFile();
        request.destination = destination;
        request.format      = format;
        request.bitrateKbps = safe->ctx.settings.getMp3BitrateKbps();

        safe->exportInProgress = true;
        safe->exportButton.setEnabled (false);
        showTransient (*safe.getComponent(), safe->statusLabel, jp ("書き出しています…"), false, 60000);

        Mp3Exporter::exportAsync (request, [safe] (Mp3Exporter::Outcome outcome)
        {
            if (safe == nullptr)
                return;

            safe->exportInProgress = false;
            safe->exportButton.setEnabled (true);

            const bool ok = outcome.result == Mp3Exporter::Result::success
                         || outcome.result == Mp3Exporter::Result::successWithWavFallback;

            showTransient (*safe.getComponent(), safe->statusLabel,
                           outcome.japaneseMessage.isNotEmpty() ? outcome.japaneseMessage
                                                                : juce::String (ok ? jp ("保存しました") : jp ("書き出しに失敗しました")),
                           ! ok, 10000);

            // reveal はメッセージスレッド（STA）から。失敗しても書き出し失敗にはしない。
            if (ok && outcome.writtenFile.existsAsFile())
                platform::revealFileInExplorer (outcome.writtenFile);
        });
    });
}

//==============================================================================
void MainComponent::openAdvancedSettings()
{
    juce::Component::SafePointer<MainComponent> safe (this);

    AdvancedSettingsDialog::launchAsync (this, ctx.settings, ctx.engine, ctx.params, ctx.hotkey,
                                         [safe]
    {
        if (safe == nullptr)
            return;

        // 詳細設定は自分でエンジンを作り直す（ノイズ除去の品質は遅延が変わるため）。
        // ここでは UI を現在値へ同期し直すだけにとどめる。
        safe->deviceSelector.setAdvancedBackendsVisible (safe->ctx.settings.getAllowAsio()
                                                          || safe->ctx.settings.getAllowExclusive());
        safe->noiseSwitch.setToggleState (safe->ctx.params.noiseSuppressionEnabled.load (std::memory_order_relaxed),
                                          juce::dontSendNotification);
        safe->exportButton.setButtonText (safe->ctx.settings.getExportFormat() == Mp3Exporter::Format::wav
                                            ? jp ("書き出し（WAV）") : jp ("書き出し（MP3）"));
        safe->refreshVoiceControls();
        safe->refreshMuteUi();
        safe->refreshLatencyDisplay();
    });
}

void MainComponent::openHotkeyDialog()
{
    juce::Component::SafePointer<MainComponent> safe (this);

    HotkeyCaptureDialog::launchAsync (this, ctx.hotkey, ctx.settings.getHotkeyBinding(),
                                      [safe] (bool accepted, HotkeyBinding binding)
    {
        if (safe == nullptr || ! accepted)
            return;

        safe->ctx.settings.setHotkeyBinding (binding);
        safe->ctx.settings.scheduleSave();

        if (! safe->ctx.hotkey.isInstalled())
        {
            const auto result = safe->ctx.hotkey.install (binding);

            if (result != GlobalHotkey::InstallResult::ok)
                showTransient (*safe.getComponent(), safe->statusLabel,
                               GlobalHotkey::japaneseError (result), true, 20000);
        }

        safe->refreshMuteUi();
    });
}

//==============================================================================
namespace
{
    /** コンボの項目 ID からプラグイン識別子を引くためのキー。
        DeviceSelectorPanel と同じく、対応表はウィジェット自身に持たせる。 */
    juce::Identifier pluginItemKey (int itemId)
    {
        return juce::Identifier ("kvcPlugin" + juce::String (itemId));
    }
}

void MainComponent::refreshCharacterLabel()
{
    if (analyser == nullptr)
        return;

    const auto r = analyser->getResult();

    if (! r.valid)
    {
        characterLabel.setText (jp ("ーーー"), juce::dontSendNotification);
        characterLabel.setColour (juce::Label::textColourId, cTextDim);
        return;
    }

    // 男性寄り=緑 / 女性寄り=ピンク / 中性=グレー。
    const juce::Colour masculine { 0xff2ed573 };
    const juce::Colour feminine  { 0xffff7eb6 };
    const juce::Colour neutral   { 0xffb0b6be };

    juce::Colour colour = neutral;

    switch (r.tone)
    {
        case VoiceAnalyser::Tone::masculine: colour = masculine; break;
        case VoiceAnalyser::Tone::feminine:  colour = feminine;  break;
        case VoiceAnalyser::Tone::neutral:
        default:                             colour = neutral;   break;
    }

    // 1 位と 2 位が僅差のときは少し淡くして「迷っている」ことを見せる。
    characterLabel.setColour (juce::Label::textColourId,
                              colour.withMultipliedAlpha (0.55f + 0.45f * r.confidence));

    // 中性（グレー）は「男女どちらとも言い切れなかった」結果なので、
    // 言い切っているように見えないよう語尾に「？」を付ける。
    const bool uncertain = (r.tone == VoiceAnalyser::Tone::neutral);

    characterLabel.setText (uncertain ? r.label + jp ("？") : r.label,
                            juce::dontSendNotification);
}

void MainComponent::rebuildInputChannelCombo()
{
    const auto status = ctx.engine.getStatus();
    const auto& names = status.input.availableInputChannels;

    inputChannelCombo.clear (juce::dontSendNotification);

    if (names.isEmpty())
    {
        inputChannelCombo.setTextWhenNoChoicesAvailable (jp ("（起動後に選べます）"));
        inputChannelCombo.setEnabled (false);
        return;
    }

    inputChannelCombo.setEnabled (true);
    inputChannelCombo.addItem (jp ("先頭2chをミックス"), 1);

    for (int i = 0; i < names.size(); ++i)
        inputChannelCombo.addItem (names[i], i + 2);

    const int wanted = ctx.settings.getInputChannelIndex();
    const int id = (wanted < 0 || wanted >= names.size()) ? 1 : wanted + 2;

    inputChannelCombo.setSelectedId (id, juce::dontSendNotification);
}

void MainComponent::rebuildEngineCombo()
{
    for (int i = 0; i < engineCombo.getNumItems(); ++i)
        engineCombo.getProperties().remove (pluginItemKey (engineCombo.getItemId (i)));

    engineCombo.clear (juce::dontSendNotification);
    engineCombo.addItem (jp ("内蔵（追加インストール不要）"), 1);
    engineCombo.getProperties().set (pluginItemKey (1), juce::String());

    const auto wanted = ctx.settings.getPluginIdentifier();

    int nextId = 2;
    bool wantedListed = false;

    // いま読み込んでいるプラグインは、走査結果に載っていなくても必ず出す。
    // 走査は任意（数秒かかる）なので、これが無いと設定済みのプラグインが
    // コンボに現れず、黙って内蔵へ戻ったように見える。
    if (plugin != nullptr && wanted.isNotEmpty())
    {
        engineCombo.addSeparator();
        engineCombo.addItem (plugin->getName() + jp ("  [使用中]"), nextId);
        engineCombo.getProperties().set (pluginItemKey (nextId), wanted);
        ++nextId;
        wantedListed = true;
    }

    if (pluginHost != nullptr)
    {
        const auto entries = pluginHost->getEntries();

        if (! entries.isEmpty())
            engineCombo.addSeparator();

        for (const auto& e : entries)
        {
            if (wantedListed && e.identifier == wanted)
                continue;   // 「使用中」として既に出している

            engineCombo.addItem (e.name + "  [" + e.format + "]", nextId);
            engineCombo.getProperties().set (pluginItemKey (nextId), e.identifier);
            ++nextId;
        }
    }

    // 保存済みの選択を復元する。
    int selected = 1;

    if (ctx.settings.getEngineKind() == Settings::EngineKind::plugin && wanted.isNotEmpty())
    {
        for (int i = 0; i < engineCombo.getNumItems(); ++i)
        {
            const int id = engineCombo.getItemId (i);

            if (engineCombo.getProperties().getWithDefault (pluginItemKey (id), juce::String()).toString() == wanted)
            {
                selected = id;
                break;
            }
        }
    }

    // 設定はプラグインを指しているのに読み込めなかった場合、黙って内蔵に戻ると
    // 「効いていない」だけに見える。理由が分かる項目を出して選択しておく。
    if (plugin == nullptr && wanted.isNotEmpty() && lastPluginError.isNotEmpty())
    {
        engineCombo.addSeparator();
        engineCombo.addItem (jp ("⚠ プラグインを読み込めませんでした"), 999);
        engineCombo.getProperties().set (pluginItemKey (999), juce::String());
        engineCombo.setTooltip (lastPluginError);
        selected = 999;
    }

    engineCombo.setSelectedId (selected, juce::dontSendNotification);

    pluginEditorButton.setEnabled (plugin != nullptr && plugin->hasEditor());
}

void MainComponent::engineSelectionChanged()
{
    const int id = engineCombo.getSelectedId();
    const auto identifier = engineCombo.getProperties()
                                .getWithDefault (pluginItemKey (id), juce::String()).toString();

    // 差し替える前に、いま使っているプラグインの設定を保存しておく。
    if (plugin != nullptr)
        ctx.settings.setPluginState (plugin->saveState());

    if (identifier.isEmpty())
    {
        ctx.settings.setEngineKind (Settings::EngineKind::builtin);
        ctx.settings.setPluginIdentifier ({});
    }
    else
    {
        ctx.settings.setEngineKind (Settings::EngineKind::plugin);
        ctx.settings.setPluginIdentifier (identifier);
        ctx.settings.setPluginState ({});   // 別のプラグインの状態を流し込まない
    }

    ctx.settings.scheduleSave();

    // エンジンを止めてから差し替える。実行中に遅延が変わってはならないため。
    ctx.engine.stop();
    closePluginEditor();
    plugin.reset();

    loadPluginFromSettings();
    rebuildEngineCombo();
    restartEngineFromUi();
}

void MainComponent::loadPluginFromSettings()
{
    if (ctx.settings.getEngineKind() != Settings::EngineKind::plugin)
        return;

    const auto identifier = ctx.settings.getPluginIdentifier();

    if (identifier.isEmpty() || pluginHost == nullptr)
        return;

    const double rate  = ctx.settings.getRequestedSampleRate() > 0.0
                             ? ctx.settings.getRequestedSampleRate() : kPreferredSampleRate;
    const int    block = ctx.settings.getRequestedBlockSize() > 0
                             ? ctx.settings.getRequestedBlockSize() : 512;

    juce::String error;
    plugin = pluginHost->load (identifier, rate, block, error);

    if (plugin == nullptr)
    {
        // 読み込めないプラグインで無音になるより、内蔵で音を出す方がまし。
        // ただし設定は書き換えない。ユーザーの選択を黙って消すと、原因を直したあと
        // 選び直しが必要になり、「勝手に戻った」という体験になる。
        lastPluginError = error;
        showTransient (*this, statusLabel, error, true, 30000);
        return;
    }

    lastPluginError.clear();

    const auto state = ctx.settings.getPluginState();

    if (state.getSize() > 0)
        plugin->restoreState (state);
}

void MainComponent::rescanPlugins()
{
    if (pluginHost == nullptr)
        return;

    // 走査はプラグインを 1 つずつ実際に読み込むので数秒かかる。
    // 落ちるプラグインがあっても次回起動時に除外できるよう、記録は PluginHost が持つ。
    pluginRescanButton.setEnabled (false);
    showTransient (*this, statusLabel, jp ("プラグインを探しています…"), false, 60000);

    juce::MouseCursor::showWaitCursor();
    pluginHost->scan();
    juce::MouseCursor::hideWaitCursor();

    rebuildEngineCombo();
    pluginRescanButton.setEnabled (true);

    const int found = pluginHost->getEntries().size();
    showTransient (*this, statusLabel,
                   jp ("プラグインを ") + juce::String (found) + jp (" 個見つけました。"), false, 8000);
}

void MainComponent::openPluginEditor()
{
    if (plugin == nullptr || ! plugin->hasEditor())
    {
        showTransient (*this, statusLabel, jp ("このプラグインは専用画面を持っていません。"), false, 6000);
        return;
    }

    if (pluginWindow != nullptr)
    {
        pluginWindow->toFront (true);
        return;
    }

    auto* editor = plugin->createEditor();

    if (editor == nullptr)
        return;

    class PluginWindow final : public juce::DocumentWindow
    {
    public:
        PluginWindow (const juce::String& title, MainComponent& ownerIn)
            : juce::DocumentWindow (title, juce::Colours::black, juce::DocumentWindow::closeButton),
              owner (ownerIn)
        {
            setUsingNativeTitleBar (true);
        }

        // エディタはプラグイン本体より先に閉じなければならない。
        void closeButtonPressed() override { owner.closePluginEditor(); }

    private:
        MainComponent& owner;
    };

    auto window = std::make_unique<PluginWindow> (plugin->getName(), *this);
    window->setContentOwned (editor, true);
    window->centreWithSize (editor->getWidth(), editor->getHeight());
    window->setVisible (true);

    pluginWindow = std::move (window);
}

void MainComponent::closePluginEditor()
{
    pluginWindow.reset();
}

} // namespace kvc
