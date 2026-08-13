// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include <juce_gui_extra/juce_gui_extra.h>

#include "app/Settings.h"
#include "audio/AudioEngine.h"
#include "audio/DeviceCatalog.h"
#include "audio/Recorder.h"
#include "core/Parameters.h"
#include "platform/GlobalHotkey_Win32.h"
#include "ui/MainComponent.h"

#include <memory>

#ifndef KVC_VERSION_STRING
 #define KVC_VERSION_STRING "1.0.1"
#endif

namespace kvc
{

//==============================================================================
namespace Colours
{
    inline const juce::Colour background { 0xff16191d };
    inline const juce::Colour panel      { 0xff1e2227 };
    inline const juce::Colour accent     { 0xff2ed573 };
    inline const juce::Colour danger     { 0xffff4757 };
    inline const juce::Colour text       { 0xffe8eaed };
    inline const juce::Colour textDim    { 0xff8a9099 };
}

//==============================================================================
/** 保存されたウィンドウ位置を、現在つながっているディスプレイの範囲へ引き戻す。
    モニタを 1 枚外した状態で起動すると、そのままでは画面外に取り残される。 */
static juce::Rectangle<int> clampToDisplays (juce::Rectangle<int> bounds)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    if (bounds.getWidth()  < MainComponent::kMinWidth
     || bounds.getHeight() < MainComponent::kMinHeight)
        return {};

    if (auto* display = displays.getDisplayForRect (bounds, false))
    {
        const auto area = display->userBounds.toNearestInt();

        bounds.setSize (juce::jmin (bounds.getWidth(),  area.getWidth()),
                        juce::jmin (bounds.getHeight(), area.getHeight()));

        // タイトルバーが必ず画面内に残るように寄せる。
        bounds.setPosition (juce::jlimit (area.getX(), area.getRight()  - bounds.getWidth(),  bounds.getX()),
                            juce::jlimit (area.getY(), area.getBottom() - bounds.getHeight(), bounds.getY()));
        return bounds;
    }

    return {};
}

//==============================================================================
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& title, AppContext context, Settings& settingsRef)
        : juce::DocumentWindow (title, Colours::background, juce::DocumentWindow::allButtons),
          settings (settingsRef)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);

        auto* content = new MainComponent (context);
        main = content;
        setContentOwned (content, true);

        setResizeLimits (MainComponent::kMinWidth,  MainComponent::kMinHeight,
                         MainComponent::kMaxWidth,  MainComponent::kMaxHeight);

        const auto saved = clampToDisplays (settings.getWindowBounds());

        if (! saved.isEmpty())
            setBounds (saved);
        else
            centreWithSize (MainComponent::kDefaultWidth, MainComponent::kDefaultHeight);

        setVisible (true);
    }

    /** 破棄前に必ず呼ばれる。ここでオーディオを止めておかないと、
        コンポーネントが消えたあとにコールバックが走る余地が残る。 */
    void shutDownContent()
    {
        if (main != nullptr)
            main->prepareToShutDown();
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    void moved() override    { storeBounds(); juce::DocumentWindow::moved(); }
    void resized() override  { storeBounds(); juce::DocumentWindow::resized(); }

    MainComponent* getMainComponent() const noexcept { return main; }

private:
    void storeBounds()
    {
        if (isShowing() && ! isMinimised() && ! isFullScreen())
        {
            settings.setWindowBounds (getBounds());   // 内部で 1 秒デバウンス保存
        }
    }

    Settings& settings;
    MainComponent* main = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
class KantanVoiceChangerApplication final : public juce::JUCEApplication
{
public:
    KantanVoiceChangerApplication() = default;

    // exe 名と同じ ASCII。日本語はウィンドウタイトル側で出す。
    const juce::String getApplicationName() override    { return "KantanVoiceChanger"; }
    const juce::String getApplicationVersion() override { return KVC_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getDarkColourScheme());

        // 生成順 = 依存順。破棄は shutdown() で必ずこの逆順に行う。
        settings = std::make_unique<Settings>();
        settings->load();

        // ★ウィンドウを作る前に適用する。全体スケールは JUCE 側で
        //   フォント・枠・余白すべてに同じ倍率で効くので、個々のフォント指定を
        //   触る必要がない（触ると固定サイズの枠から文字がはみ出す箇所が必ず出る）。
        juce::Desktop::getInstance().setGlobalScaleFactor (settings->getUiScale());

        params = std::make_unique<VoiceParams>();
        settings->applyToParams (*params);

        catalog = std::make_unique<DeviceCatalog>();
        catalog->rescan();

        recorder = std::make_unique<Recorder>();

        engine = std::make_unique<AudioEngine> (*catalog, *params);
        engine->setRecorderTap (recorder.get());   // エンジン停止中に 1 回だけ

        hotkey = std::make_unique<GlobalHotkey>();

        AppContext context { *settings, *params, *catalog, *engine, *recorder, *hotkey };

        mainWindow = std::make_unique<MainWindow> (juce::String::fromUTF8 ("簡単ボイチェン"),
                                                   context, *settings);

        // デバイスを開くのはウィンドウが出たあと。開けなかった理由を UI に出せるようにするため。
        if (auto* main = mainWindow->getMainComponent())
            main->applySettingsAndStartEngine();
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->shutDownContent();   // エンジン停止・録音停止・設定確定

        mainWindow.reset();

        // 依存の逆順。ホットキーのフックはウィンドウより先に外す。
        if (hotkey != nullptr)  hotkey->uninstall();
        hotkey.reset();

        if (engine != nullptr)  engine->stop();
        engine.reset();

        recorder.reset();
        catalog.reset();

        if (settings != nullptr)
        {
            if (params != nullptr)
                settings->captureFromParams (*params);

            settings->saveIfNeeded();
        }

        params.reset();
        settings.reset();

        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setVisible (true);
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }
    }

    /** ログオフ・シャットダウン（WM_ENDSESSION 相当）。デバウンスを待たずに書く。 */
    void suspended() override
    {
        if (settings != nullptr)
            settings->saveIfNeeded();
    }

private:
    juce::LookAndFeel_V4 lookAndFeel;

    std::unique_ptr<Settings>      settings;
    std::unique_ptr<VoiceParams>   params;
    std::unique_ptr<DeviceCatalog> catalog;
    std::unique_ptr<Recorder>      recorder;
    std::unique_ptr<AudioEngine>   engine;
    std::unique_ptr<GlobalHotkey>  hotkey;
    std::unique_ptr<MainWindow>    mainWindow;
};

} // namespace kvc

//==============================================================================
START_JUCE_APPLICATION (kvc::KantanVoiceChangerApplication)
