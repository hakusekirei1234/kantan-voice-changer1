// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "ui/HotkeyCaptureDialog.h"

#include <memory>
#include <utility>


namespace
{
    // JUCE の String(const char*) は ASCII 専用（CharPointer_ASCII）。
    // UTF-8 の日本語リテラルは必ずこれを通す。通さないと 1 バイトずつ別文字に化ける。
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

namespace kvc
{

namespace
{
    const juce::Colour kPanel   { 0xff1e2227 };
    const juce::Colour kText    { 0xffe8eaed };
    const juce::Colour kTextDim { 0xff8a9099 };
    const juce::Colour kAccent  { 0xff2ed573 };
    const juce::Colour kDanger  { 0xffff4757 };
    const juce::Colour kWarn    { 0xffffa502 };

    juce::Font uiFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions().withHeight (height)
                                              .withStyle (bold ? "Bold" : "Regular"));
    }

    struct SafeKeyItem
    {
        int          id;
        juce::String label;
        uint32_t     vk;
        uint32_t     mods;
    };

    // JIS 配列で単独割り当てしても他アプリの入力を壊さないキーだけを並べる。
    const SafeKeyItem kSafeKeys[] =
    {
        { 1, jp ("無変換"),       SafeKeys::vkNonConvert, HotkeyMods::none },
        { 2, jp ("変換"),         SafeKeys::vkConvert,    HotkeyMods::none },
        { 3, "Pause",        SafeKeys::vkPause,      HotkeyMods::none },
        { 4, "F9",           SafeKeys::vkF9,         HotkeyMods::none },
        { 5, "F12",          SafeKeys::vkF12,        HotkeyMods::none },
        { 6, "Ctrl+Shift+M", static_cast<uint32_t> ('M'),
                             HotkeyMods::control | HotkeyMods::shift }
    };

    constexpr int kCaptureSeconds = 5;
    constexpr int kModeRadioGroup = 0x6b7c;
}

//==============================================================================
HotkeyCaptureDialog::HotkeyCaptureDialog (GlobalHotkey& hk,
                                          HotkeyBinding current,
                                          CompletionCallback cb)
    : hotkey (hk),
      original (current),
      working (current),
      onComplete (std::move (cb))
{
    setOpaque (true);
    setSize (500, 438);

    auto initLabel = [this] (juce::Label& l, float height, juce::Colour colour,
                             juce::Justification just, bool bold)
    {
        l.setFont (uiFont (height, bold));
        l.setColour (juce::Label::textColourId, colour);
        l.setJustificationType (just);
        l.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (l);
    };

    initLabel (currentLabel, 17.0f, kText,    juce::Justification::centredLeft, true);
    initLabel (warningLabel, 12.5f, kTextDim, juce::Justification::topLeft,     false);
    initLabel (confirmLabel, 13.0f, kTextDim, juce::Justification::topLeft,     false);

    changeButton.onClick = [this] { beginCapture(); };
    addAndMakeVisible (changeButton);

    safeKeyCombo.setTextWhenNothingSelected (jp ("「安全なキー」から選ぶ"));
    safeKeyCombo.setTextWhenNoChoicesAvailable (jp ("選択肢がありません"));

    for (const auto& s : kSafeKeys)
        safeKeyCombo.addItem (s.label, s.id);

    safeKeyCombo.onChange = [this]
    {
        const int id = safeKeyCombo.getSelectedId();

        for (const auto& s : kSafeKeys)
        {
            if (s.id == id)
            {
                working.vk = s.vk;
                working.mods = s.mods;
                applied = false;
                confirmed = false;
                refreshUi();
                confirmLabel.setColour (juce::Label::textColourId, kTextDim);
                confirmLabel.setText (jp ("「決定」を押すと、このキーが有効になります。"),
                                      juce::dontSendNotification);
                break;
            }
        }
    };
    addAndMakeVisible (safeKeyCombo);

    modeToggle.setButtonText (jp ("押すたびに切り替え（トグル）"));
    modeToggle.setRadioGroupId (kModeRadioGroup);
    modeToggle.onClick = [this]
    {
        if (! modeToggle.getToggleState())
            return;

        working.mode = HotkeyMode::toggle;
        applied = false;
        confirmed = false;
        refreshUi();
    };
    addAndMakeVisible (modeToggle);

    pushToMuteToggle.setButtonText (jp ("押している間だけミュート"));
    pushToMuteToggle.setRadioGroupId (kModeRadioGroup);
    pushToMuteToggle.onClick = [this]
    {
        if (! pushToMuteToggle.getToggleState())
            return;

        working.mode = HotkeyMode::pushToMute;
        applied = false;
        confirmed = false;
        refreshUi();
    };
    addAndMakeVisible (pushToMuteToggle);

    passThroughToggle.setButtonText (jp ("他のアプリでもこのキーを使えるようにする（推奨）"));
    passThroughToggle.onClick = [this]
    {
        working.passThrough = passThroughToggle.getToggleState();
        applied = false;
        confirmed = false;
        refreshUi();
    };
    addAndMakeVisible (passThroughToggle);

    okButton.onClick = [this]
    {
        if (applied)
            finish (true);
        else
            applyAndTest();
    };
    addAndMakeVisible (okButton);

    cancelButton.onClick = [this] { finish (false); };
    addAndMakeVisible (cancelButton);

    refreshUi();
    confirmLabel.setText (jp ("「変更」を押すか、上の「安全なキー」から選んでください。"),
                          juce::dontSendNotification);
}

HotkeyCaptureDialog::~HotkeyCaptureDialog()
{
    capture.end();
    hotkey.removeListener (this);

    // 何も確定していない状態で閉じられた（Esc / ウィンドウの × など）。
    // ここで戻しておかないと、アプリがホットキー無しのまま取り残される。
    if (! applied)
    {
        hotkey.uninstall();
        hotkey.install (original);
    }

    if (onComplete != nullptr)
    {
        auto cb = std::move (onComplete);
        onComplete = nullptr;
        cb (false, original);
    }
}

//==============================================================================
void HotkeyCaptureDialog::launchAsync (juce::Component* parentForCentring,
                                       GlobalHotkey& hk,
                                       HotkeyBinding current,
                                       CompletionCallback cb)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new HotkeyCaptureDialog (hk, current, std::move (cb)));
    o.dialogTitle = jp ("ミュートのショートカットキー");
    o.dialogBackgroundColour = kPanel;
    o.componentToCentreAround = parentForCentring;
    o.useNativeTitleBar = true;
    o.resizable = false;

    // Esc はキャプチャ中止の合図としてフック側が使う。ここで閉じるボタンに
    // 割り当てると、キーを取り込んでいる途中に Esc を押しただけで
    // ダイアログごと消えてしまう。
    o.escapeKeyTriggersCloseButton = false;

    o.launchAsync();
}

//==============================================================================
void HotkeyCaptureDialog::paint (juce::Graphics& g)
{
    g.fillAll (kPanel);

    g.setColour (juce::Colour (0xff2a2f36));
    const int y = getHeight() - 56;
    g.drawHorizontalLine (y, 16.0f, static_cast<float> (getWidth() - 16));
}

void HotkeyCaptureDialog::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto buttons = r.removeFromBottom (32);
    okButton.setBounds (buttons.removeFromRight (120));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (110));

    r.removeFromBottom (16);

    auto row = r.removeFromTop (32);
    changeButton.setBounds (row.removeFromRight (96));
    row.removeFromRight (10);
    currentLabel.setBounds (row);

    r.removeFromTop (14);
    safeKeyCombo.setBounds (r.removeFromTop (30));

    r.removeFromTop (16);
    modeToggle.setBounds (r.removeFromTop (24));
    r.removeFromTop (4);
    pushToMuteToggle.setBounds (r.removeFromTop (24));

    r.removeFromTop (10);
    passThroughToggle.setBounds (r.removeFromTop (24));

    r.removeFromTop (12);
    warningLabel.setBounds (r.removeFromTop (100));

    r.removeFromTop (6);
    confirmLabel.setBounds (r.removeFromTop (44));
}

//==============================================================================
void HotkeyCaptureDialog::refreshUi()
{
    const bool capturing = capture.isActive();

    currentLabel.setColour (juce::Label::textColourId, capturing ? kWarn : kText);
    currentLabel.setText (capturing ? jp ("設定したいキーを押してください")
                                    : jp ("現在: ") + GlobalHotkey::describeBinding (working),
                          juce::dontSendNotification);

    changeButton.setButtonText (capturing ? jp ("受付中…") : jp ("変更"));
    changeButton.setEnabled (! capturing);
    safeKeyCombo.setEnabled (! capturing);
    modeToggle.setEnabled (! capturing);
    pushToMuteToggle.setEnabled (! capturing);
    passThroughToggle.setEnabled (! capturing);
    okButton.setEnabled (! capturing);

    modeToggle.setToggleState (working.mode == HotkeyMode::toggle, juce::dontSendNotification);
    pushToMuteToggle.setToggleState (working.mode == HotkeyMode::pushToMute, juce::dontSendNotification);
    passThroughToggle.setToggleState (working.passThrough, juce::dontSendNotification);

    int matchedId = 0;

    for (const auto& s : kSafeKeys)
    {
        if (s.vk == working.vk && s.mods == working.mods)
        {
            matchedId = s.id;
            break;
        }
    }

    safeKeyCombo.setSelectedId (matchedId, juce::dontSendNotification);

    juce::String warning;
    juce::Colour warningColour = kTextDim;

    if (GlobalHotkey::isForbiddenBinding (working))
    {
        warning = jp ("Enter や Space の単独指定は、ダイアログやゲームの操作と衝突するため"
                  "設定できません。別のキーを選んでください。");
        warningColour = kDanger;
    }
    else if (GlobalHotkey::isRiskyBareKey (working))
    {
        warning = jp ("⚠ 「V」のように文字キーだけを設定すると、ほかのアプリで文字を入力する"
                  "たびにミュートが切り替わってしまいます。Ctrl や Shift と組み合わせるか、"
                  "「無変換」キーや F9 などをおすすめします。\n"
                  "おすすめのキーにするには、上の「安全なキー」から選んでください。"
                  "このまま使う場合は「決定」を押してください。");
        warningColour = kDanger;
    }
    else if (GlobalHotkey::chooseBackend (working) == GlobalHotkey::BackendKind::lowLevelHook)
    {
        warning = jp ("この設定では、キーボードを監視する方式でキーを受け取ります。"
                  "単独キーや「押している間だけミュート」を使うために必要ですが、"
                  "セキュリティソフトが警告を出すことがあります。");
        warningColour = kWarn;
    }

    warningLabel.setColour (juce::Label::textColourId, warningColour);
    warningLabel.setText (warning, juce::dontSendNotification);

    if (capturing)
    {
        confirmLabel.setColour (juce::Label::textColourId, kWarn);
        confirmLabel.setText (jp ("あと ") + juce::String (captureSecondsLeft)
                                + jp (" 秒。Esc を押すと取り消します。\n"
                                  "受付中は、押したキーは他のアプリには送られません。"),
                              juce::dontSendNotification);
    }
    else if (confirmed)
    {
        confirmLabel.setColour (juce::Label::textColourId, kAccent);
        confirmLabel.setText (jp ("✓ 反応しました。このキーで正しく動いています。"),
                              juce::dontSendNotification);
    }

    okButton.setButtonText (applied ? jp ("閉じる") : jp ("決定"));
    cancelButton.setButtonText (applied ? jp ("取り消す") : jp ("キャンセル"));
}

//==============================================================================
void HotkeyCaptureDialog::beginCapture()
{
    if (capture.isActive())
        return;

    // 手順 1: 先に本番のバインドを外す。付けたままキャプチャすると、
    // ユーザーが押したキーで実際にミュートが切り替わってしまう。
    hotkey.removeListener (this);
    hotkey.uninstall();
    applied = false;
    confirmed = false;

    if (! capture.begin (this))
    {
        restoreLiveBinding();
        refreshUi();
        warningLabel.setColour (juce::Label::textColourId, kDanger);
        warningLabel.setText (jp ("キーの読み取りを開始できませんでした。"
                              "ほかのアプリがキーボードを占有している可能性があります。"
                              "「安全なキー」から選ぶ方法もお使いいただけます。"),
                              juce::dontSendNotification);
        return;
    }

    captureSecondsLeft = kCaptureSeconds;
    refreshUi();

    juce::Component::SafePointer<HotkeyCaptureDialog> safe (this);

    // 再帰ラムダにすると自分自身を握る循環参照が残るので、5 本まとめて予約する。
    // 前回のキャプチャの残りタイマーは残り秒数の連続性で弾く。
    for (int i = 1; i <= kCaptureSeconds; ++i)
    {
        juce::Timer::callAfterDelay (i * 1000, [safe, i]
        {
            if (safe == nullptr || ! safe->capture.isActive())
                return;

            if (safe->captureSecondsLeft != kCaptureSeconds - i + 1)
                return;

            safe->captureSecondsLeft = kCaptureSeconds - i;

            if (safe->captureSecondsLeft <= 0)
                safe->endCapture (jp ("時間切れです。もう一度「変更」を押してください。"));
            else
                safe->refreshUi();
        });
    }
}

void HotkeyCaptureDialog::endCapture (const juce::String& statusJapanese)
{
    capture.end();
    captureSecondsLeft = 0;
    restoreLiveBinding();
    refreshUi();

    confirmLabel.setColour (juce::Label::textColourId, kTextDim);
    confirmLabel.setText (statusJapanese, juce::dontSendNotification);
}

void HotkeyCaptureDialog::restoreLiveBinding()
{
    hotkey.uninstall();
    hotkey.install (applied ? working : original);
}

//==============================================================================
void HotkeyCaptureDialog::keyCaptured (uint32_t vk, uint32_t mods)
{
    HotkeyBinding candidate = working;
    candidate.vk = vk;
    candidate.mods = mods;

    if (GlobalHotkey::isForbiddenBinding (candidate))
    {
        endCapture (jp ("このキーは設定できません。もう一度「変更」を押して、別のキーを押してください。"));
        warningLabel.setColour (juce::Label::textColourId, kDanger);
        warningLabel.setText (jp ("Enter や Space の単独指定は、ダイアログやゲームの操作と衝突するため"
                              "設定できません。"),
                              juce::dontSendNotification);
        return;
    }

    working = candidate;
    applied = false;
    confirmed = false;

    // 単独文字キーはパススルーを強制する。切ると、そのキーが Windows 全体で
    // 効かなくなり、ユーザーはキーボードが壊れたと思い込む。
    if (GlobalHotkey::isRiskyBareKey (working))
        working.passThrough = true;

    endCapture (jp ("「決定」を押すと、このキーが有効になります。"));
}

void HotkeyCaptureDialog::captureCancelled()
{
    endCapture (jp ("取り消しました。設定は変わっていません。"));
}

void HotkeyCaptureDialog::hotkeyPressed()
{
    if (! applied || confirmed)
        return;

    confirmed = true;
    refreshUi();
}

//==============================================================================
void HotkeyCaptureDialog::applyAndTest()
{
    if (GlobalHotkey::isForbiddenBinding (working))
    {
        refreshUi();
        return;
    }

    hotkey.uninstall();
    const auto result = hotkey.install (working);

    if (result != GlobalHotkey::InstallResult::ok)
    {
        applied = false;
        confirmed = false;

        // 失敗したままにするとホットキーが一切無い状態で残る。必ず元に戻す。
        hotkey.uninstall();
        hotkey.install (original);

        refreshUi();
        warningLabel.setColour (juce::Label::textColourId, kDanger);
        warningLabel.setText (GlobalHotkey::japaneseError (result), juce::dontSendNotification);

        confirmLabel.setColour (juce::Label::textColourId, kTextDim);
        confirmLabel.setText (jp ("設定できませんでした。別のキーを選んでください。"),
                              juce::dontSendNotification);
        return;
    }

    applied = true;
    confirmed = false;
    hotkey.addListener (this);

    refreshUi();
    confirmLabel.setColour (juce::Label::textColourId, kWarn);
    confirmLabel.setText (jp ("設定しました。実際に ") + GlobalHotkey::describeBinding (working)
                            + jp (" を押して、反応するか確かめてください。"),
                          juce::dontSendNotification);
}

void HotkeyCaptureDialog::finish (bool accepted)
{
    capture.end();
    hotkey.removeListener (this);

    const bool keep = accepted && applied;

    if (! keep)
    {
        hotkey.uninstall();
        hotkey.install (original);
    }

    if (onComplete != nullptr)
    {
        auto cb = std::move (onComplete);
        onComplete = nullptr;
        cb (keep, keep ? working : original);
    }

    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);
}

} // namespace kvc
