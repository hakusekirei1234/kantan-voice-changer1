// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "ui/AdvancedSettingsDialog.h"

#include <memory>
#include <utility>


namespace
{
    // JUCE の String(const char*) は ASCII 専用（CharPointer_ASCII）。
    // UTF-8 の日本語リテラルは必ずこれを通す。通さないと 1 バイトずつ別文字に化ける。
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

#ifndef KVC_VERSION_STRING
 #define KVC_VERSION_STRING "1.0.1"
#endif

#ifndef KVC_ASIO_ENABLED
 #define KVC_ASIO_ENABLED 0
#endif

namespace kvc
{

namespace
{
    const juce::Colour kPanel   { 0xff1e2227 };
    const juce::Colour kSunken  { 0xff101317 };
    const juce::Colour kOutline { 0xff2a2f36 };
    const juce::Colour kText    { 0xffe8eaed };
    const juce::Colour kTextDim { 0xff8a9099 };
    const juce::Colour kAccent  { 0xff2ed573 };
    const juce::Colour kDanger  { 0xffff4757 };
    const juce::Colour kWarn    { 0xffffa502 };

    constexpr int kCaptionWidth = 176;
    constexpr int kInfoTabIndex = 5;

    juce::Font uiFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions().withHeight (height)
                                              .withStyle (bold ? "Bold" : "Regular"));
    }

    void initCaption (juce::Component& parent, juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (uiFont (14.0f));
        l.setColour (juce::Label::textColourId, kText);
        l.setJustificationType (juce::Justification::centredLeft);
        l.setMinimumHorizontalScale (1.0f);
        parent.addAndMakeVisible (l);
    }

    void initNote (juce::Component& parent, juce::Label& l, const juce::String& text,
                   juce::Colour colour = kTextDim)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (uiFont (11.5f));
        l.setColour (juce::Label::textColourId, colour);
        l.setJustificationType (juce::Justification::topLeft);
        l.setMinimumHorizontalScale (1.0f);
        parent.addAndMakeVisible (l);
    }

    void placeLabelled (juce::Rectangle<int> row, juce::Component& caption, juce::Component& control)
    {
        caption.setBounds (row.removeFromLeft (kCaptionWidth));
        control.setBounds (row);
    }

    juce::String msText (double samples, double rate)
    {
        const double sr = rate > 0.0 ? rate : kPreferredSampleRate;
        return juce::String (samples * 1000.0 / sr, 1) + " ms";
    }

    //==========================================================================
    /** タブの中身の共通土台。背景はダイアログ側が塗るので透明のまま。 */
    class PageBase : public juce::Component
    {
    public:
        PageBase() { setOpaque (false); }

    protected:
        static juce::Rectangle<int> contentArea (juce::Rectangle<int> local)
        {
            return local.reduced (18, 16);
        }
    };

    //==========================================================================
    // 表示
    //==========================================================================
    class DisplayPage final : public PageBase
    {
    public:
        explicit DisplayPage (Settings& s) : settings (s)
        {
            initCaption (*this, capScale, jp ("表示サイズ"));

            // JUCE の全体スケールで拡大する。個々のフォント指定を触ると
            // 固定サイズの枠から文字がはみ出す箇所が必ず出るので、そうしない。
            scaleCombo.addItem (jp ("標準（100%）"), 1);
            scaleCombo.addItem (jp ("大きめ（115%）"), 2);
            scaleCombo.addItem (jp ("さらに大きめ（130%）"), 3);
            scaleCombo.addItem (jp ("最大（150%）"), 4);

            scaleCombo.onChange = [this]
            {
                const float table[] = { 1.0f, 1.15f, 1.30f, 1.50f };
                const int id = juce::jlimit (1, 4, scaleCombo.getSelectedId());

                settings.setUiScale (table[id - 1]);
                juce::Desktop::getInstance().setGlobalScaleFactor (table[id - 1]);
            };

            addAndMakeVisible (scaleCombo);

            initNote (*this, noteScale,
                      jp ("画面全体の大きさを変えます。文字も枠も同じ倍率で拡大されるので、"
                      "レイアウトが崩れることはありません。すぐに反映されます。"
                      "大きくするとウィンドウ自体も大きくなるので、"
                      "画面からはみ出す場合は一段下げてください。"));
        }

        void load()
        {
            const float s = settings.getUiScale();
            const int id = s >= 1.45f ? 4 : s >= 1.25f ? 3 : s >= 1.08f ? 2 : 1;

            scaleCombo.setSelectedId (id, juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = contentArea (getLocalBounds());

            placeLabelled (r.removeFromTop (28), capScale, scaleCombo);
            noteScale.setBounds (r.removeFromTop (60));
        }

    private:
        Settings& settings;
        juce::Label capScale, noteScale;
        juce::ComboBox scaleCombo;
    };

    //==========================================================================
    // オーディオ
    //==========================================================================
    class AudioPage final : public PageBase
    {
    public:
        AudioPage (Settings& s, VoiceParams& p)
            : settings (s), params (p)
        {
            initCaption (*this, capBuffer, jp ("バッファサイズ"));
            bufferCombo.addItem (jp ("自動（機器におまかせ）"), 1);

            for (int i = 0; i < 5; ++i)
            {
                const int n = 64 << i;
                bufferCombo.addItem (juce::String (n) + jp (" サンプル（約 ")
                                       + msText (n, kPreferredSampleRate) + jp ("）"),
                                     2 + i);
            }

            bufferCombo.onChange = [this]
            {
                const int id = bufferCombo.getSelectedId();
                settings.setRequestedBlockSize (id <= 1 ? 0 : (64 << (id - 2)));
                markGraphDirty();
            };
            addAndMakeVisible (bufferCombo);
            initNote (*this, noteBuffer,
                      jp ("小さいほど遅れが減りますが、音が途切れやすくなります。"
                      "迷ったら 128 サンプルのままにしてください。"));

            initCaption (*this, capRate, jp ("サンプルレート"));
            rateCombo.addItem (jp ("自動（48000 Hz を優先）"), 1);
            rateCombo.addItem ("44100 Hz", 2);
            rateCombo.addItem ("48000 Hz", 3);
            rateCombo.onChange = [this]
            {
                const int id = rateCombo.getSelectedId();
                settings.setRequestedSampleRate (id == 2 ? 44100.0 : (id == 3 ? 48000.0 : 0.0));
                markGraphDirty();
            };
            addAndMakeVisible (rateCombo);
            initNote (*this, noteRate,
                      jp ("機器どうしのレートが違うと、ずれ補正が上限に張り付いて音が途切れます。"
                      "「情報」タブで確認できます。"));

            initCaption (*this, capFill, jp ("送信バッファ"));
            fillCombo.addItem (jp ("安定優先（推奨）"), 1);
            fillCombo.addItem (jp ("低遅延優先"), 2);
            fillCombo.onChange = [this]
            {
                settings.setFillMode (fillCombo.getSelectedId() == 2
                                        ? DriftRing::FillMode::lowLatency
                                        : DriftRing::FillMode::stable);
                markGraphDirty();
            };
            addAndMakeVisible (fillCombo);
            initNote (*this, noteFill,
                      jp ("モニターと送信で別の機器を使う場合、機器ごとに時計が違うため"
                      "13〜24 ms 程度の補正バッファが必ず入ります。これは省略できません。"));

            initCaption (*this, capInGain, jp ("マイク入力の音量"));
            initGainSlider (inGain);
            inGain.onValueChange = [this]
            {
                const auto db = static_cast<float> (inGain.getValue());
                params.inputGainDb.store (db, std::memory_order_relaxed);
                settings.setInputGainDb (db);
            };
            addAndMakeVisible (inGain);

            initCaption (*this, capOutGain, jp ("出力音量"));
            initGainSlider (outGain);
            outGain.onValueChange = [this]
            {
                const auto db = static_cast<float> (outGain.getValue());
                params.outputGainDb.store (db, std::memory_order_relaxed);
                settings.setOutputGainDb (db);
            };
            addAndMakeVisible (outGain);
            initNote (*this, noteGain,
                      jp ("出力音量は、自分に聞こえる音と相手に送る音の両方に効きます。"));

            muteMonitorToggle.setButtonText (jp ("ミュート中は自分の声も止める"));
            muteMonitorToggle.onClick = [this]
            {
                const bool v = muteMonitorToggle.getToggleState();
                params.muteAlsoSilencesMonitor.store (v, std::memory_order_relaxed);
                settings.setMuteAlsoSilencesMonitor (v);
            };
            addAndMakeVisible (muteMonitorToggle);
            initNote (*this, noteMute,
                      jp ("OFF のままだと、ミュート中でも自分の声は聞こえ続けます"
                      "（相手には送られません）。マイクが生きているか確認できるので、"
                      "こちらをおすすめします。"));

            characterToggle.setButtonText (jp ("声の特徴を自動判別（ベータ）"));
            characterToggle.onClick = [this]
            {
                settings.setCharacterEnabled (characterToggle.getToggleState());
            };
            addAndMakeVisible (characterToggle);
            initNote (*this, noteCharacter,
                      jp ("画面上部に「いまの声の特徴」をテキストで出します。"
                      "学習済みAIではなく音響的な目安なので、外れることがよくあります。"
                      "はっきりしない判定には語尾に「？」が付きます。"));

            asioToggle.setButtonText (jp ("ASIO の機器も候補に出す"));
            asioToggle.onClick = [this]
            {
                settings.setAllowAsio (asioToggle.getToggleState());
                markGraphDirty();
            };
            addAndMakeVisible (asioToggle);

           #if KVC_ASIO_ENABLED
            initNote (*this, noteAsio,
                      jp ("SYNCROOM など他のアプリが同じ ASIO ドライバを使っていると、"
                      "あとから起動した方が失敗します。うまくいかないときは OFF に戻してください。"),
                      kWarn);
           #else
            asioToggle.setEnabled (false);
            initNote (*this, noteAsio, jp ("このアプリは ASIO 無効でビルドされています。"), kTextDim);
           #endif

            exclusiveToggle.setButtonText (jp ("排他モードの機器も候補に出す"));
            exclusiveToggle.onClick = [this]
            {
                settings.setAllowExclusive (exclusiveToggle.getToggleState());
                markGraphDirty();
            };
            addAndMakeVisible (exclusiveToggle);
            initNote (*this, noteExclusive,
                      jp ("排他モードは他のアプリから同じ機器を使えなくします。"
                      "SYNCROOM や配信ソフトと併用する場合は OFF のままにしてください。"),
                      kWarn);

            initNote (*this, noteRestart,
                      jp ("★ バッファサイズ・サンプルレート・送信バッファ・ASIO・排他モードを変えると、"
                      "このウィンドウを閉じたときにオーディオを作り直します。"
                      "そのとき一瞬だけ音が途切れます。"),
                      kWarn);

            syncFromSettings();
        }

        void syncFromSettings()
        {
            const int block = settings.getRequestedBlockSize();
            int blockId = 1;

            for (int i = 0; i < 5; ++i)
                if (block == (64 << i))
                    blockId = 2 + i;

            bufferCombo.setSelectedId (blockId, juce::dontSendNotification);

            const double rate = settings.getRequestedSampleRate();
            rateCombo.setSelectedId (rate == 44100.0 ? 2 : (rate == 48000.0 ? 3 : 1),
                                     juce::dontSendNotification);

            fillCombo.setSelectedId (settings.getFillMode() == DriftRing::FillMode::lowLatency ? 2 : 1,
                                     juce::dontSendNotification);

            inGain.setValue (settings.getInputGainDb(), juce::dontSendNotification);
            outGain.setValue (settings.getOutputGainDb(), juce::dontSendNotification);

            muteMonitorToggle.setToggleState (settings.getMuteAlsoSilencesMonitor(),
                                              juce::dontSendNotification);
            asioToggle.setToggleState (settings.getAllowAsio(), juce::dontSendNotification);
            exclusiveToggle.setToggleState (settings.getAllowExclusive(), juce::dontSendNotification);
            characterToggle.setToggleState (settings.getCharacterEnabled(), juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = contentArea (getLocalBounds());

            placeLabelled (r.removeFromTop (28), capBuffer, bufferCombo);
            noteBuffer.setBounds (r.removeFromTop (30));
            r.removeFromTop (6);

            placeLabelled (r.removeFromTop (28), capRate, rateCombo);
            noteRate.setBounds (r.removeFromTop (30));
            r.removeFromTop (6);

            placeLabelled (r.removeFromTop (28), capFill, fillCombo);
            noteFill.setBounds (r.removeFromTop (30));
            r.removeFromTop (6);

            placeLabelled (r.removeFromTop (26), capInGain, inGain);
            r.removeFromTop (2);
            placeLabelled (r.removeFromTop (26), capOutGain, outGain);
            noteGain.setBounds (r.removeFromTop (18));
            r.removeFromTop (8);

            muteMonitorToggle.setBounds (r.removeFromTop (24));
            noteMute.setBounds (r.removeFromTop (32));
            r.removeFromTop (6);

            characterToggle.setBounds (r.removeFromTop (24));
            noteCharacter.setBounds (r.removeFromTop (32));
            r.removeFromTop (6);

            asioToggle.setBounds (r.removeFromTop (24));
            noteAsio.setBounds (r.removeFromTop (30));
            r.removeFromTop (4);

            exclusiveToggle.setBounds (r.removeFromTop (24));
            noteExclusive.setBounds (r.removeFromTop (30));
            r.removeFromTop (6);

            noteRestart.setBounds (r.removeFromTop (44));
        }

    private:
        void initGainSlider (juce::Slider& s)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setRange (-24.0, 12.0, 0.5);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 68, 20);
            s.setTextValueSuffix (" dB");
        }

        /** ここで書いた値は GraphRequest の中身なので、エンジンを組み直さないと効かない。
            組み直しはウィンドウを閉じたときに呼び出し側がまとめて行う。コンボを
            いじるたびにデバイスを開き直すと、その都度音が途切れて操作にならない。 */
        void markGraphDirty() {}

        Settings&    settings;
        VoiceParams& params;

        juce::Label    capBuffer, capRate, capFill, capInGain, capOutGain;
        juce::ComboBox bufferCombo, rateCombo, fillCombo;
        juce::Slider   inGain, outGain;
        juce::ToggleButton muteMonitorToggle, asioToggle, exclusiveToggle, characterToggle;
        juce::Label    noteBuffer, noteRate, noteFill, noteGain, noteMute,
                       noteAsio, noteExclusive, noteRestart, noteCharacter;
    };

    //==========================================================================
    // キー設定
    //==========================================================================
    class HotkeyPage final : public PageBase
    {
    public:
        HotkeyPage (Settings& s, GlobalHotkey& h)
            : settings (s), hotkey (h)
        {
            initCaption (*this, caption, jp ("ミュートのショートカットキー"));

            keyLabel.setFont (uiFont (20.0f, true));
            keyLabel.setColour (juce::Label::textColourId, kAccent);
            keyLabel.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (keyLabel);

            changeButton.setButtonText (jp ("変更"));
            changeButton.onClick = [this] { openCaptureDialog(); };
            addAndMakeVisible (changeButton);

            passThroughToggle.setButtonText (jp ("他のアプリでもこのキーを使えるようにする（推奨）"));
            passThroughToggle.onClick = [this]
            {
                auto b = settings.getHotkeyBinding();
                b.passThrough = passThroughToggle.getToggleState();
                settings.setHotkeyBinding (b);

                hotkey.uninstall();
                const auto r = hotkey.install (b);

                if (r != GlobalHotkey::InstallResult::ok)
                    setStatus (GlobalHotkey::japaneseError (r), kDanger);
                else
                    setStatus ({}, kTextDim);

                refresh();
            };
            addAndMakeVisible (passThroughToggle);

            initNote (*this, backendNote, {});
            initNote (*this, statusNote, {});
            initNote (*this, generalNote,
                      jp ("ミュートは「相手への送信だけ」を止めます。自分の声は聞こえ続けるので、"
                      "マイクが生きているかいつでも確認できます。\n"
                      "この動作を変えたい場合は「オーディオ」タブの"
                      "「ミュート中は自分の声も止める」を ON にしてください。\n\n"
                      "ゲームや管理者権限のウィンドウが前面にあるときは、Windows の仕組み上"
                      "ショートカットキーが届かないことがあります。"));

            refresh();
        }

        void refresh()
        {
            const auto b = settings.getHotkeyBinding();
            keyLabel.setText (GlobalHotkey::describeBinding (b), juce::dontSendNotification);
            passThroughToggle.setToggleState (b.passThrough, juce::dontSendNotification);

            juce::String backend;

            switch (hotkey.getActiveBackend())
            {
                case GlobalHotkey::BackendKind::registerHotKey:
                    backend = jp ("受け取り方式: Windows のショートカット登録（他のアプリの入力経路に入りません）");
                    break;
                case GlobalHotkey::BackendKind::lowLevelHook:
                    backend = jp ("受け取り方式: キーボード監視（単独キーや「押している間だけ」に必要です。"
                              "セキュリティソフトが警告を出すことがあります）");
                    break;
                case GlobalHotkey::BackendKind::none:
                default:
                    backend = jp ("受け取り方式: 未登録（ショートカットキーは効いていません）");
                    break;
            }

            backendNote.setText (backend, juce::dontSendNotification);
            backendNote.setColour (juce::Label::textColourId,
                                   hotkey.isInstalled() ? kTextDim : kDanger);

            const auto mode = b.mode == HotkeyMode::pushToMute
                                ? juce::String (jp ("動作: 押している間だけミュート"))
                                : juce::String (jp ("動作: 押すたびに切り替え（トグル）"));

            caption.setText (jp ("ミュートのショートカットキー   —   ") + mode, juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = contentArea (getLocalBounds());

            caption.setBounds (r.removeFromTop (22));
            r.removeFromTop (6);

            auto row = r.removeFromTop (40);
            changeButton.setBounds (row.removeFromRight (96).reduced (0, 4));
            row.removeFromRight (10);
            keyLabel.setBounds (row);

            r.removeFromTop (10);
            backendNote.setBounds (r.removeFromTop (32));
            r.removeFromTop (8);

            passThroughToggle.setBounds (r.removeFromTop (24));
            r.removeFromTop (8);

            statusNote.setBounds (r.removeFromTop (30));
            r.removeFromTop (10);

            generalNote.setBounds (r.removeFromTop (130));
        }

    private:
        void setStatus (const juce::String& text, juce::Colour colour)
        {
            statusNote.setColour (juce::Label::textColourId, colour);
            statusNote.setText (text, juce::dontSendNotification);
        }

        void openCaptureDialog()
        {
            juce::Component::SafePointer<HotkeyPage> safe (this);

            HotkeyCaptureDialog::launchAsync (this, hotkey, settings.getHotkeyBinding(),
                                              [safe] (bool accepted, HotkeyBinding binding)
            {
                if (safe == nullptr)
                    return;

                if (accepted)
                {
                    safe->settings.setHotkeyBinding (binding);
                    safe->setStatus (jp ("新しいキーを保存しました。"), kAccent);
                }

                safe->refresh();
            });
        }

        Settings&     settings;
        GlobalHotkey& hotkey;

        juce::Label      caption, keyLabel;
        juce::TextButton changeButton;
        juce::ToggleButton passThroughToggle;
        juce::Label      backendNote, statusNote, generalNote;
    };

    //==========================================================================
    // 音質
    //==========================================================================
    class QualityPage final : public PageBase
    {
    public:
        QualityPage (Settings& s, VoiceParams& p, AudioEngine& e)
            : settings (s), params (p), engine (e)
        {
            initCaption (*this, capQuality, jp ("ノイズ除去の品質"));
            qualityCombo.addItem (jp ("低遅延（+ ") + msText (kNoiseFftLowLatency, kPreferredSampleRate)
                                    + jp ("）  おすすめ"), 1);
            qualityCombo.addItem (jp ("高音質（+ ") + msText (kNoiseFftHighQuality, kPreferredSampleRate)
                                    + jp ("）"), 2);
            qualityCombo.onChange = [this]
            {
                const auto q = qualityCombo.getSelectedId() == 2 ? NoiseQuality::highQuality512
                                                                 : NoiseQuality::lowLatency256;
                settings.setNoiseQuality (q);
                params.noiseQuality.store (static_cast<int> (q), std::memory_order_relaxed);

                // 品質は VoiceEngine::prepare でしか読まれず、変えるとアルゴリズム遅延が
                // 変わる。動作中に遅延が変わってはならないので、必ずグラフを作り直す。
                engine.restartAsync();

                updateLatencyNote();
            };
            addAndMakeVisible (qualityCombo);

            initNote (*this, noteQuality,
                      jp ("★ 品質を変えると音の遅れそのものが変わるため、オーディオを作り直します。"
                      "動作中に遅れが変わることは無い設計なので、切り替えの瞬間だけ音が途切れます。"),
                      kWarn);

            initCaption (*this, capStrength, jp ("ノイズ除去の強さ"));
            strengthCombo.addItem (jp ("弱（最大 6 dB）"), 1);
            strengthCombo.addItem (jp ("標準（最大 12 dB）"), 2);
            strengthCombo.addItem (jp ("強（最大 18 dB）"), 3);
            strengthCombo.onChange = [this]
            {
                const auto st = static_cast<NoiseStrength> (
                                    juce::jlimit (0, 2, strengthCombo.getSelectedId() - 1));
                settings.setNoiseStrength (st);
                params.noiseStrength.store (static_cast<int> (st), std::memory_order_relaxed);
            };
            addAndMakeVisible (strengthCombo);

            initNote (*this, noteStrength,
                      jp ("強くするほど無音区間は静かになりますが、声の細かい部分も削れて"
                      "こもった感じになります。強さの変更は即座に反映されます。"));

            initNote (*this, noteScope,
                      jp ("消せるのは、扇風機・PC のファン・エアコン・マイクのサーというノイズのような"
                      "「ずっと鳴り続けている音」だけです。キーボードの打鍵音、マウスのクリック、"
                      "紙のこすれる音、ドアの音はほとんど消えません。"));

            initNote (*this, noteLatency, {});

            initNote (*this, notePitchLatency,
                      jp ("声の高さ・声質の変換は、ON にしても OFF にしても遅れは変わりません"
                      "（先読み 512 サンプルを常に固定しているためです）。"
                      "切り替えのたびに遅れが変わると、そのつどプチッと鳴ってしまいます。"));

            syncFromSettings();
        }

        void syncFromSettings()
        {
            qualityCombo.setSelectedId (settings.getNoiseQuality() == NoiseQuality::highQuality512 ? 2 : 1,
                                        juce::dontSendNotification);
            strengthCombo.setSelectedId (static_cast<int> (settings.getNoiseStrength()) + 1,
                                         juce::dontSendNotification);
            updateLatencyNote();
        }

        void resized() override
        {
            auto r = contentArea (getLocalBounds());

            placeLabelled (r.removeFromTop (28), capQuality, qualityCombo);
            noteQuality.setBounds (r.removeFromTop (44));
            r.removeFromTop (6);

            noteLatency.setBounds (r.removeFromTop (60));
            r.removeFromTop (10);

            placeLabelled (r.removeFromTop (28), capStrength, strengthCombo);
            noteStrength.setBounds (r.removeFromTop (32));
            r.removeFromTop (8);

            noteScope.setBounds (r.removeFromTop (54));
            r.removeFromTop (10);

            notePitchLatency.setBounds (r.removeFromTop (54));
        }

    private:
        void updateLatencyNote()
        {
            const auto q = settings.getNoiseQuality();
            const int fft = fftSizeForQuality (q);
            const int total = dspLatencySamples (q);

            juce::String s;
            s << jp ("この設定での処理の遅れ（機器のバッファは含みません）\n")
              << jp ("    声の変換（PSOLA 先読み）: ") << kPsolaLookaheadSamples << jp (" サンプル / ")
              << msText (kPsolaLookaheadSamples, kPreferredSampleRate) << "\n"
              << jp ("    ノイズ除去（STFT N=") << fft << jp ("）: ") << fft << jp (" サンプル / ")
              << msText (fft, kPreferredSampleRate) << "\n"
              << jp ("    合計: ") << total << jp (" サンプル / ") << msText (total, kPreferredSampleRate);

            noteLatency.setText (s, juce::dontSendNotification);
            noteLatency.setColour (juce::Label::textColourId, kText);
        }

        Settings&    settings;
        VoiceParams& params;
        AudioEngine& engine;

        juce::Label    capQuality, capStrength;
        juce::ComboBox qualityCombo, strengthCombo;
        juce::Label    noteQuality, noteStrength, noteScope, noteLatency, notePitchLatency;
    };

    //==========================================================================
    // 通知音
    //==========================================================================
    class BeepPage final : public PageBase
    {
    public:
        BeepPage (Settings& s, VoiceParams& p, AudioEngine& e)
            : settings (s), params (p), engine (e)
        {
            enableToggle.setButtonText (jp ("ミュートを切り替えたときに音を鳴らす"));
            enableToggle.onClick = [this]
            {
                const bool v = enableToggle.getToggleState();
                params.beepEnabled.store (v, std::memory_order_relaxed);
                engine.getBeepGenerator().setEnabled (v);
                settings.setBeepEnabled (v);
                volumeSlider.setEnabled (v);
                testButton.setEnabled (v);
            };
            addAndMakeVisible (enableToggle);

            initCaption (*this, capVolume, jp ("音量"));
            volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            volumeSlider.setRange (0.0, 100.0, 1.0);
            volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
            volumeSlider.setTextValueSuffix (" %");
            volumeSlider.onValueChange = [this]
            {
                const auto v = static_cast<float> (volumeSlider.getValue() * 0.01);
                params.beepVolume.store (v, std::memory_order_relaxed);
                engine.getBeepGenerator().setVolume (v);
                settings.setBeepVolume (v);
            };
            addAndMakeVisible (volumeSlider);

            testButton.setButtonText (jp ("鳴らしてみる"));
            testButton.onClick = [this]
            {
                // trigger はシーケンス番号を進めるだけ。オーディオ側が次のブロックで鳴らす。
                engine.getBeepGenerator().trigger (BeepGenerator::Kind::muteOn);
            };
            addAndMakeVisible (testButton);

            initNote (*this, note1,
                      jp ("この音はあなたにだけ聞こえ、相手には送られません。"),
                      kAccent);

            initNote (*this, note2,
                      jp ("録音した音や書き出したファイルにも入りません。"
                      "通知音はモニター（自分に聞こえる音）にだけ足される構造になっています。\n\n"
                      "ミュート ON は 1200 Hz、ミュート OFF は 800 Hz の 60 ms の短い音です。"
                      "ウィンドウを隠していても、この音とタスクトレイのアイコンで"
                      "ミュート状態が分かります。"));

            syncFromSettings();
        }

        void syncFromSettings()
        {
            const bool on = settings.getBeepEnabled();
            enableToggle.setToggleState (on, juce::dontSendNotification);
            volumeSlider.setValue (settings.getBeepVolume() * 100.0, juce::dontSendNotification);
            volumeSlider.setEnabled (on);
            testButton.setEnabled (on);
        }

        void resized() override
        {
            auto r = contentArea (getLocalBounds());

            enableToggle.setBounds (r.removeFromTop (24));
            r.removeFromTop (12);

            auto row = r.removeFromTop (26);
            testButton.setBounds (row.removeFromRight (110));
            row.removeFromRight (12);
            placeLabelled (row, capVolume, volumeSlider);

            r.removeFromTop (16);
            note1.setBounds (r.removeFromTop (20));
            r.removeFromTop (8);
            note2.setBounds (r.removeFromTop (140));
        }

    private:
        Settings&    settings;
        VoiceParams& params;
        AudioEngine& engine;

        juce::ToggleButton enableToggle;
        juce::Label        capVolume;
        juce::Slider       volumeSlider;
        juce::TextButton   testButton;
        juce::Label        note1, note2;
    };

    //==========================================================================
    // 書き出し
    //==========================================================================
    class ExportPage final : public PageBase
    {
    public:
        explicit ExportPage (Settings& s)
            : settings (s), mp3Available (Mp3Exporter::probeMp3Available())
        {
            initCaption (*this, capFormat, jp ("形式"));
            formatCombo.addItem ("MP3", 1);
            formatCombo.addItem (jp ("WAV（無圧縮）"), 2);
            formatCombo.onChange = [this]
            {
                settings.setExportFormat (formatCombo.getSelectedId() == 2
                                            ? Mp3Exporter::Format::wav
                                            : Mp3Exporter::Format::mp3);
                updateEnablement();
            };
            addAndMakeVisible (formatCombo);

            initCaption (*this, capBitrate, jp ("MP3 の音質"));
            bitrateCombo.addItem (jp ("128 kbps（小さいファイル）"), 1);
            bitrateCombo.addItem (jp ("192 kbps（おすすめ）"), 2);
            bitrateCombo.addItem (jp ("320 kbps（高音質）"), 3);
            bitrateCombo.onChange = [this]
            {
                const int id = bitrateCombo.getSelectedId();
                settings.setMp3BitrateKbps (id == 1 ? 128 : (id == 3 ? 320 : 192));
            };
            addAndMakeVisible (bitrateCombo);

            initCaption (*this, capFolder, jp ("保存先フォルダ"));
            folderLabel.setFont (uiFont (12.0f));
            folderLabel.setColour (juce::Label::textColourId, kText);
            folderLabel.setColour (juce::Label::backgroundColourId, kSunken);
            folderLabel.setJustificationType (juce::Justification::centredLeft);
            folderLabel.setMinimumHorizontalScale (0.6f);
            addAndMakeVisible (folderLabel);

            browseButton.setButtonText (jp ("変更"));
            browseButton.onClick = [this] { chooseFolder(); };
            addAndMakeVisible (browseButton);

            openFolderButton.setButtonText (jp ("開く"));
            openFolderButton.onClick = [this]
            {
                platform::openFolder (settings.getExportFolder());
            };
            addAndMakeVisible (openFolderButton);

            initNote (*this, noteMp3, {});
            initNote (*this, noteFolder,
                      jp ("クラウド同期フォルダ（Synology Drive / OneDrive など）を保存先にすると、"
                      "書き出し中のファイルを同期クライアントが掴んで失敗することがあります。"
                      "既定のミュージックフォルダのままをおすすめします。"));

            syncFromSettings();
        }

        void syncFromSettings()
        {
            formatCombo.setSelectedId (settings.getExportFormat() == Mp3Exporter::Format::wav ? 2 : 1,
                                       juce::dontSendNotification);

            const int kbps = settings.getMp3BitrateKbps();
            bitrateCombo.setSelectedId (kbps == 128 ? 1 : (kbps == 320 ? 3 : 2),
                                        juce::dontSendNotification);

            folderLabel.setText (" " + settings.getExportFolder().getFullPathName(),
                                 juce::dontSendNotification);

            updateEnablement();
        }

        void resized() override
        {
            auto r = contentArea (getLocalBounds());

            placeLabelled (r.removeFromTop (28), capFormat, formatCombo);
            r.removeFromTop (6);
            noteMp3.setBounds (r.removeFromTop (46));
            r.removeFromTop (6);

            placeLabelled (r.removeFromTop (28), capBitrate, bitrateCombo);
            r.removeFromTop (14);

            capFolder.setBounds (r.removeFromTop (22));
            r.removeFromTop (2);

            auto row = r.removeFromTop (28);
            openFolderButton.setBounds (row.removeFromRight (72));
            row.removeFromRight (6);
            browseButton.setBounds (row.removeFromRight (72));
            row.removeFromRight (8);
            folderLabel.setBounds (row);

            r.removeFromTop (10);
            noteFolder.setBounds (r.removeFromTop (58));
        }

    private:
        void updateEnablement()
        {
            const bool wantMp3 = settings.getExportFormat() == Mp3Exporter::Format::mp3;

            if (! mp3Available)
            {
                // N 版や Media Feature Pack 未導入の環境。選ばせない。
                formatCombo.setItemEnabled (1, false);

                if (wantMp3)
                {
                    settings.setExportFormat (Mp3Exporter::Format::wav);
                    formatCombo.setSelectedId (2, juce::dontSendNotification);
                }

                noteMp3.setColour (juce::Label::textColourId, kDanger);
                noteMp3.setText (jp ("この Windows には MP3 のエンコーダーがありません（N 版など）。"
                                 "Microsoft の「メディア機能パック」を入れると MP3 で保存できるように"
                                 "なります。それまでは WAV で保存されます。"),
                                 juce::dontSendNotification);
            }
            else
            {
                noteMp3.setColour (juce::Label::textColourId, kTextDim);
                noteMp3.setText (jp ("MP3 は Windows 標準のエンコーダーを使います。"
                                 "録音は 1 チャンネル（モノラル）なので、128 kbps でも十分な音質です。"),
                                 juce::dontSendNotification);
            }

            bitrateCombo.setEnabled (mp3Available
                                       && settings.getExportFormat() == Mp3Exporter::Format::mp3);
        }

        void chooseFolder()
        {
            chooser = std::make_unique<juce::FileChooser> (jp ("保存先フォルダを選んでください"),
                                                           settings.getExportFolder());

            juce::Component::SafePointer<ExportPage> safe (this);

            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                  [safe] (const juce::FileChooser& fc)
            {
                if (safe == nullptr)
                    return;

                const auto folder = fc.getResult();

                if (folder.isDirectory())
                {
                    safe->settings.setExportFolder (folder);
                    safe->syncFromSettings();
                }
            });
        }

        Settings& settings;
        const bool mp3Available;

        juce::Label    capFormat, capBitrate, capFolder, folderLabel;
        juce::ComboBox formatCombo, bitrateCombo;
        juce::TextButton browseButton, openFolderButton;
        juce::Label    noteMp3, noteFolder;

        std::unique_ptr<juce::FileChooser> chooser;   // 非モーダル。寿命を持たせる必要がある。
    };

    //==========================================================================
    // 情報 — 現地デバッグの唯一の手段。ここだけは削らない。
    //==========================================================================
    class InfoPage final : public PageBase
    {
    public:
        InfoPage (Settings& s, AudioEngine& e, GlobalHotkey& h)
            : settings (s), engine (e), hotkey (h)
        {
            readout.setMultiLine (true, false);
            readout.setReadOnly (true);
            readout.setScrollbarsShown (true);
            readout.setCaretVisible (false);
            readout.setPopupMenuEnabled (true);
            // 等幅フォントにすると桁は揃うが、Windows の等幅フォントは日本語グリフを
            // 持たないものが多く豆腐になる。既定フォントのままにする。
            readout.setFont (uiFont (12.5f));
            readout.setColour (juce::TextEditor::backgroundColourId, kSunken);
            readout.setColour (juce::TextEditor::outlineColourId, kOutline);
            readout.setColour (juce::TextEditor::focusedOutlineColourId, kOutline);
            readout.setColour (juce::TextEditor::textColourId, kText);
            addAndMakeVisible (readout);

            copyButton.setButtonText (jp ("コピー"));
            copyButton.onClick = [this]
            {
                juce::SystemClipboard::copyTextToClipboard (readout.getText());
                setStatus (jp ("この内容をクリップボードにコピーしました。サポートへの連絡にお使いください。"),
                           kAccent);
            };
            addAndMakeVisible (copyButton);

            logFolderButton.setButtonText (jp ("ログフォルダを開く"));
            logFolderButton.onClick = [this]
            {
                const auto folder = settings.getSettingsFolder();

                if (! folder.isDirectory())
                    folder.createDirectory();

                if (! platform::openFolder (folder))
                    setStatus (jp ("フォルダを開けませんでした: ") + folder.getFullPathName(), kDanger);
            };
            addAndMakeVisible (logFolderButton);

            resetButton.setButtonText (jp ("設定をリセット"));
            resetButton.onClick = [this] { handleReset(); };
            addAndMakeVisible (resetButton);

            initNote (*this, statusNote, {});

            refresh();
        }

        void refresh()
        {
            const auto text = buildReport();

            if (text == lastText)
                return;

            lastText = text;

            // 1 秒ごとに setText するとキャレットが先頭に戻る。位置だけ戻しておく。
            const int caret = readout.getCaretPosition();
            readout.setText (text, false);
            readout.setCaretPosition (juce::jmin (caret, text.length()));
        }

        void resized() override
        {
            auto r = contentArea (getLocalBounds());

            auto bottom = r.removeFromBottom (28);
            resetButton.setBounds (bottom.removeFromRight (120));
            bottom.removeFromRight (8);
            logFolderButton.setBounds (bottom.removeFromRight (140));
            bottom.removeFromRight (8);
            copyButton.setBounds (bottom.removeFromRight (80));

            r.removeFromBottom (8);
            statusNote.setBounds (r.removeFromBottom (30));
            r.removeFromBottom (6);

            readout.setBounds (r);
        }

    private:
        void setStatus (const juce::String& text, juce::Colour colour)
        {
            statusNote.setColour (juce::Label::textColourId, colour);
            statusNote.setText (text, juce::dontSendNotification);
        }

        void handleReset()
        {
            // JUCE_MODAL_LOOPS_PERMITTED=0 なので確認ダイアログは出さず、
            // 2 段階クリックで誤操作を防ぐ。
            if (! resetArmed)
            {
                resetArmed = true;
                resetButton.setButtonText (jp ("本当にリセット"));
                resetButton.setColour (juce::TextButton::buttonColourId, kDanger);
                setStatus (jp ("もう一度押すと、機器の選択・キー割り当てを含む全ての設定が初期値に戻ります。"),
                           kWarn);
                return;
            }

            resetArmed = false;
            resetButton.setButtonText (jp ("設定をリセット"));
            resetButton.removeColour (juce::TextButton::buttonColourId);

            settings.resetToDefaults();
            setStatus (jp ("設定を初期値に戻しました。完全に反映するにはアプリを再起動してください。"),
                       kAccent);
        }

        static juce::String describeLeg (const juce::String& caption, const AudioEngine::LegStatus& l)
        {
            juce::String s;
            s << caption << ": ";

            if (! l.active)
            {
                s << jp ("未使用\n");
                return s;
            }

            s << l.deviceName << "\n"
              << "        " << backendDisplayName (l.backend)
              << " / " << juce::String (l.sampleRate, 0) << " Hz"
              << " / " << l.blockSize << jp (" サンプル")
              << jp (" / 機器遅延 ") << l.deviceLatencySamples << jp (" サンプル")
              << (l.latencyReportedByDriver ? "" : jp ("（推定）")) << "\n";
            return s;
        }

        static juce::String describeRing (const juce::String& caption, bool present,
                                          const DriftRing::Diagnostics& d, double rate)
        {
            juce::String s;
            s << caption << ": ";

            if (! present)
            {
                s << jp ("なし（同じ機器 / 同じ時計なので補正不要）\n");
                return s;
            }

            s << (d.ppm >= 0.0f ? "+" : "") << juce::String (d.ppm, 1) << " ppm"
              << jp (" / たまり ") << d.fill << jp (" (平均 ") << juce::String (d.avgFill, 1)
              << jp (" / 目標 ") << d.targetFill << " = " << msText (d.targetFill, rate) << ")"
              << jp ("\n        途切れ ") << static_cast<int> (d.underruns) << jp (" 回")
              << jp (" / あふれ ") << static_cast<int> (d.overruns) << jp (" 回\n");

            if (d.pinnedAtClamp)
                s << jp ("        ⚠ 補正が上限に張り付いています。機器どうしのサンプルレートが"
                     "合っていない可能性が高いです。\n");

            return s;
        }

        juce::String buildReport() const
        {
            const auto st = engine.getStatus();
            const double rate = st.input.sampleRate > 0.0 ? st.input.sampleRate : kPreferredSampleRate;

            juce::String s;
            s << jp ("簡単ボイチェン  バージョン ") << KVC_VERSION_STRING << "\n";
            s << juce::String::repeatedString ("-", 58) << "\n";

            s << jp ("状態: ") << (st.running ? jp ("動作中") : jp ("停止中")) << "\n";
            s << jp ("構成: ") << (st.collapsed
                                ? jp ("入力とモニターを 1 台にまとめています（いちばん遅れが少ない構成）")
                                : jp ("入力・モニター・送信が別々の機器です"))
              << "\n";

            if (st.lastErrorJapanese.isNotEmpty())
                s << jp ("直近のエラー: ") << st.lastErrorJapanese << "\n";

            s << jp ("\n【機器】\n");
            s << describeLeg (jp ("  入力    "), st.input);
            s << describeLeg (jp ("  モニター"), st.monitor);
            s << describeLeg (jp ("  送信    "), st.send);

            const int fft = juce::jmax (0, st.dspLatencySamples - kPsolaLookaheadSamples);

            s << jp ("\n【音の遅れ】\n");
            s << jp ("  声の変換（先読み）  : ") << kPsolaLookaheadSamples << jp (" サンプル / ")
              << msText (kPsolaLookaheadSamples, rate) << "\n";
            s << jp ("  ノイズ除去（STFT）  : ") << fft << jp (" サンプル / ") << msText (fft, rate) << "\n";
            s << jp ("  処理の合計          : ") << st.dspLatencySamples << jp (" サンプル / ")
              << msText (st.dspLatencySamples, rate) << "\n";
            s << jp ("  モニター往復 合計   : ") << st.monitorTotalLatencySamples << jp (" サンプル / ")
              << juce::String (st.monitorTotalLatencyMs, 1) << " ms"
              << (st.latencyIsEstimate ? jp ("（推定値）") : "") << "\n";
            s << jp ("  ※ 声の高さ・声質を OFF にしても、この値は変わりません。\n");

            s << jp ("\n【ずれ補正（3 つの機器の時計の差を吸収しています）】\n");
            s << describeRing (jp ("  モニター"), engine.hasMonitorRing(),
                               engine.getMonitorDiagnostics(),
                               st.monitor.sampleRate > 0.0 ? st.monitor.sampleRate : rate);
            s << describeRing (jp ("  送信    "), engine.hasSendRing(),
                               engine.getSendDiagnostics(),
                               st.send.sampleRate > 0.0 ? st.send.sampleRate : rate);

            s << jp ("\n【その他】\n");
            s << jp ("  ショートカットキー: ") << GlobalHotkey::describeBinding (settings.getHotkeyBinding());

            switch (hotkey.getActiveBackend())
            {
                case GlobalHotkey::BackendKind::registerHotKey: s << jp ("（ショートカット登録）\n"); break;
                case GlobalHotkey::BackendKind::lowLevelHook:   s << jp ("（キーボード監視）\n"); break;
                case GlobalHotkey::BackendKind::none:
                default:                                        s << jp ("（未登録）\n"); break;
            }

            s << jp ("  MP3 エンコーダー  : ")
              << (Mp3Exporter::probeMp3Available() ? jp ("利用できます") : jp ("ありません（WAV で保存されます）"))
              << "\n";
            s << "  ASIO              : "
             #if KVC_ASIO_ENABLED
              << (settings.getAllowAsio() ? jp ("有効（候補に出す）") : jp ("無効（候補に出さない）"))
             #else
              << jp ("このビルドでは無効")
             #endif
              << "\n";
            s << jp ("  設定フォルダ      : ") << settings.getSettingsFolder().getFullPathName() << "\n";

            if (engine.isSilentInputSuspected())
                s << jp ("\n⚠ 入力が 3 秒以上まったくの無音です。Windows の「マイクのプライバシー設定」で"
                     "このアプリのマイク使用が許可されているか確認してください。\n");

            return s;
        }

        Settings&     settings;
        AudioEngine&  engine;
        GlobalHotkey& hotkey;

        juce::TextEditor readout;
        juce::TextButton copyButton, logFolderButton, resetButton;
        juce::Label      statusNote;

        juce::String lastText;
        bool resetArmed = false;
    };
} // namespace

//==============================================================================
AdvancedSettingsDialog::AdvancedSettingsDialog (Settings& s, AudioEngine& e,
                                                VoiceParams& p, GlobalHotkey& h)
    : settings (s), engine (e), params (p), hotkey (h)
{
    setOpaque (true);
    setSize (560, 620);

    tabs.setOutline (0);
    tabs.setTabBarDepth (34);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, kPanel);
    tabs.setColour (juce::TabbedComponent::outlineColourId, kOutline);
    addAndMakeVisible (tabs);

    const auto tabColour = kPanel;

    tabs.addTab (jp ("オーディオ"), tabColour, new AudioPage   (settings, params),          true);
    tabs.addTab (jp ("キー設定"),   tabColour, new HotkeyPage  (settings, hotkey),          true);
    tabs.addTab (jp ("音質"),       tabColour, new QualityPage (settings, params, engine),  true);
    tabs.addTab (jp ("通知音"),     tabColour, new BeepPage    (settings, params, engine),  true);
    tabs.addTab (jp ("書き出し"),   tabColour, new ExportPage  (settings),                  true);
    tabs.addTab (jp ("情報"),       tabColour, new InfoPage    (settings, engine, hotkey),  true);

    {
        auto* display = new DisplayPage (settings);
        display->load();
        tabs.addTab (jp ("表示"), tabColour, display, true);
    }

    closeButton.setButtonText (jp ("閉じる"));
    closeButton.onClick = [this]
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
    };
    addAndMakeVisible (closeButton);

    startTimer (1000);
}

AdvancedSettingsDialog::~AdvancedSettingsDialog()
{
    stopTimer();
    settings.saveIfNeeded();
}

//==============================================================================
void AdvancedSettingsDialog::launchAsync (juce::Component* parentForCentring,
                                          Settings& s, AudioEngine& e,
                                          VoiceParams& p, GlobalHotkey& h,
                                          std::function<void()> onClosed)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new AdvancedSettingsDialog (s, e, p, h));
    o.dialogTitle = jp ("詳細設定");
    o.dialogBackgroundColour = kPanel;
    o.componentToCentreAround = parentForCentring;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;

    auto* dw = o.launchAsync();

    if (dw == nullptr)
        return;

    // launchAsync は自前のモーダルコールバックを取らないので、あとから括り付ける。
    // 呼び出し側はここで Settings を読み直してグラフを組み直す。バッファサイズや
    // ノイズ除去の品質は GraphRequest の中身なので、AudioEngine::restartAsync()
    // （最後の GraphRequest をそのまま再生する）では反映されない。
    juce::ModalComponentManager::getInstance()
        ->attachCallback (dw, juce::ModalCallbackFunction::create (
                                  [cb = std::move (onClosed)] (int)
                                  {
                                      if (cb != nullptr)
                                          cb();
                                  }));
}

//==============================================================================
void AdvancedSettingsDialog::paint (juce::Graphics& g)
{
    g.fillAll (kPanel);

    g.setColour (kOutline);
    const int y = getHeight() - 48;
    g.drawHorizontalLine (y, 0.0f, static_cast<float> (getWidth()));
}

void AdvancedSettingsDialog::resized()
{
    auto r = getLocalBounds();

    auto bottom = r.removeFromBottom (48).reduced (16, 10);
    closeButton.setBounds (bottom.removeFromRight (110));

    tabs.setBounds (r);
}

//==============================================================================
void AdvancedSettingsDialog::timerCallback()
{
    // 情報タブを見ているときだけ更新する。裏で毎秒文字列を組み立てる意味は無い。
    if (tabs.getCurrentTabIndex() == kInfoTabIndex)
        refreshInfoTab();
}

void AdvancedSettingsDialog::refreshInfoTab()
{
    if (auto* info = dynamic_cast<InfoPage*> (tabs.getTabContentComponent (kInfoTabIndex)))
        info->refresh();
}

} // namespace kvc
