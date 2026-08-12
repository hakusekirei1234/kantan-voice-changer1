// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "app/Settings.h"
#include "audio/AudioEngine.h"
#include "audio/DeviceCatalog.h"
#include "audio/Mp3Exporter.h"
#include "audio/PluginHost.h"
#include "audio/Recorder.h"
#include "core/Parameters.h"
#include "core/VoiceAnalyser.h"
#include "platform/GlobalHotkey_Win32.h"
#include "platform/RevealInExplorer.h"
#include "ui/AdvancedSettingsDialog.h"
#include "ui/DeviceSelectorPanel.h"
#include "ui/HotkeyCaptureDialog.h"
#include "ui/WaveformDisplay.h"

#include <memory>

namespace kvc
{

//==============================================================================
/** Main.cpp が所有する長寿命オブジェクトへの参照束。
    MainComponent はどれも所有しない（破棄順序を Main.cpp 側に一本化するため）。 */
struct AppContext
{
    Settings&      settings;
    VoiceParams&   params;
    DeviceCatalog& catalog;
    AudioEngine&   engine;
    Recorder&      recorder;
    GlobalHotkey&  hotkey;
};

//==============================================================================
/** 900 x 640 のメイン画面。

    縦の並び（ui.json のレイアウト表に対応）:
      y   0- 60  ヘッダ       タイトル / 巨大ミュートボタン(200x44) / 状態ピル
      y  60-176  ① 機器を選ぶ  DeviceSelectorPanel
      y 176-334  波形          WaveformDisplay (860x150)
      y 334-366  入力レベル + 「遅延 約 ○○ ms」
      y 366-500  ② 声を変える  プリセットチップ 4 種 + ピッチ/フォルマントの 2 スライダー
      y 500-572  ③ 録音して確かめる  録音 / 再生 / 停止 / 経過 / 書き出し
      y 572-616  ステータスバー ノイズ除去トグル + 状態文言 + 詳細設定

    設計上ゆずれない点:
      - プリセットチップ（元の声 / 高め（女性寄り） / 低め（男性寄り） / かわいい）が
        一般ユーザーにとっての実質的な主操作。「半音」「フォルマント」は通じない。
        ここを削ると、スライダーの出来に関わらず使いにくいと評価される。
      - ミュートは「相手への送信だけを止める」。自分の声は聞こえ続ける。
        ボタンの副文には現在の割当キー名を出す（クリックで割当ダイアログへ）。
      - 遅延表示は常時。エフェクトを切っても減らないことを隠さない。
      - トレイアイコンにミュート状態を反映する（緑マイク / 赤い斜線マイク）。
        ウィンドウを隠していてもホットキーは効くので、これが唯一の視覚的手掛かりになる。
        トーストや最前面オーバーレイは出さない（ゲームや SYNCROOM の上に乗って嫌われる）。
      - 書き出しは FileChooser（非モーダル launchAsync）→ Mp3Exporter::exportAsync →
        成功したら platform::revealFileInExplorer。追加の「フォルダを開きますか？」は出さない。

    スレッド契約: 全てメッセージスレッド専用。オーディオ側の値は atomic 読みのみ。
*/
class MainComponent final : public juce::Component,
                            private DeviceSelectorPanel::Listener,
                            private AudioEngine::Listener,
                            private GlobalHotkey::Listener,
                            private juce::Timer
{
public:
    static constexpr int kDefaultWidth = 900;
    static constexpr int kDefaultHeight = 640;
    static constexpr int kMinWidth = 880;
    static constexpr int kMinHeight = 620;
    static constexpr int kMaxWidth = 1600;
    static constexpr int kMaxHeight = 1000;

    explicit MainComponent (AppContext);
    ~MainComponent() override;

    /** 起動時に設定を UI に流し込み、ホットキーを登録し、構成検証を通れば
        エンジンを起動する。ホットキー登録の失敗はステータスバーに日本語で出す。
        初回起動時は「相手に送る音」を未選択のままにして誘導文を出す。 */
    void applySettingsAndStartEngine();

    /** 終了前に呼ぶ。エンジン停止・録音停止・設定の確定保存。 */
    void prepareToShutDown();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // DeviceSelectorPanel::Listener
    void deviceSelectionChanged() override;
    void deviceRescanRequested() override;

    // AudioEngine::Listener
    void engineStatusChanged() override;
    void engineErrorOccurred (const juce::String& japaneseMessage) override;
    void engineDeviceListChanged() override;

    // GlobalHotkey::Listener
    void hotkeyPressed() override;
    void hotkeyReleased() override;
    void hotkeyBackendRecovered() override;

    void timerCallback() override;   // 経過時間・状態文言・遅延表示の更新（4 Hz）

    /** hotkeyHintLabel をクリック可能にするためだけの override。
        Label は onClick を持たないので、addMouseListener で拾う。 */
    void mouseUp (const juce::MouseEvent&) override;

    void setMuted (bool);
    void refreshMuteUi();
    void refreshLatencyDisplay();
    void refreshVoiceControls();
    void applyPreset (PresetKind);
    void restartEngineFromUi();

    void startRecording();
    void stopRecordingOrPlayback();
    void startPlayback();
    void beginExport();

    void openAdvancedSettings();
    void openHotkeyDialog();

    void rebuildEngineCombo();
    void engineSelectionChanged();
    void rescanPlugins();
    void openPluginEditor();
    void closePluginEditor();
    /** 設定に保存された識別子でプラグインを読み込む。失敗したら内蔵へ戻す。 */
    void loadPluginFromSettings();

    //==========================================================================
    AppContext ctx;

    // ヘッダ
    juce::Label      titleLabel;

    /** 声質判定の表示。男性寄り=緑 / 女性寄り=ピンク / 中性=グレー。
        一度声を出したら、無音になっても最後の判定を出したままにする
        （声を出した後に目視で確認したいという要件）。 */
    juce::Label      characterLabel, characterCaption;
    std::unique_ptr<VoiceAnalyser> analyser;
    int              analyserTickDivider = 0;
    bool             characterVisible = false;
    juce::TextButton muteButton;      ///< 200x44。未ミュート=緑枠、ミュート中=赤塗り。
    juce::Label      statePill;
    juce::Label      hotkeyHintLabel;  ///< ミュートボタン下の「○ キー」。クリックで割当変更。

    // ① 機器
    DeviceSelectorPanel deviceSelector;

    // 波形とレベル
    WaveformDisplay waveform;
    LevelMeter      levelMeter;
    juce::Label     levelLabel, clipLabel;

    /** 入力とモニターを 1 台にまとめて同期バッファを消すワンクリック操作。
        効果がある構成が存在するときだけ出す。遅延の最大の効きどころなので、
        詳細設定に埋めずにここへ置く。 */
    juce::TextButton optimiseLatencyButton;

    /** 使う入力チャンネル。オーディオインターフェースは入力を何本も出すので、
        どれにマイクが挿さっているかを利用者にしか決められない。
        MOTU M6 は ASIO で 10 本（In 1-6 / Loopback / Loopback Mix）出す。 */
    juce::Label      inputChannelLabel;
    juce::ComboBox   inputChannelCombo;

    void rebuildInputChannelCombo();
    void refreshCharacterLabel();

    // ② 声を変える
    juce::Label      voiceSectionLabel;

    /** 音声処理エンジンの選択。内蔵 PSOLA か、外部プラグイン（VST3 / VST2）か。
        プラグインは同一プロセスで動くので、差し替えは必ずエンジンを止めてから行う。 */
    juce::ComboBox   engineCombo;
    juce::TextButton pluginRescanButton, pluginEditorButton;

    juce::TextButton presetChips[4];
    juce::ToggleButton pitchEnableSwitch, formantEnableSwitch;
    juce::Label      pitchLabel, formantLabel;
    juce::Slider     pitchSlider, formantSlider;
    juce::Label      pitchValueLabel, formantValueLabel;
    juce::TextButton pitchResetButton, formantResetButton;

    // ③ 録音
    juce::Label      recordSectionLabel, recordHintLabel;
    juce::TextButton recordButton, playButton, stopButton, exportButton;
    juce::Label      timeLabel;

    // ステータスバー
    juce::ToggleButton noiseSwitch;
    juce::Label        noiseSubLabel, statusLabel;
    juce::TextButton   advancedButton;

    /** アプリ内で唯一の TooltipWindow。これが無いと setTooltip が一切表示されない。
        メインウィンドウが持つのが最も寿命が長く安全。 */
    juce::TooltipWindow tooltipWindow { nullptr, 700 };

    // トレイ
    std::unique_ptr<juce::SystemTrayIconComponent> trayIcon;

    /** プラグインの探索と読み込み。プラグイン本体は当アプリと同一プロセスで動く。
        ★破棄順が重要: pluginWindow → plugin の順。エディタはプラグインより先に閉じる。 */
    std::unique_ptr<PluginHost>  pluginHost;
    std::unique_ptr<LoadedPlugin> plugin;
    std::unique_ptr<juce::DocumentWindow> pluginWindow;

    /** 直近のプラグイン読み込み失敗の理由。コンボに理由を出すために保持する。 */
    juce::String lastPluginError;

    /** ハウリング保護の通知を出したか。解除されたら false へ戻す。 */
    bool howlWarningShown = false;

    bool exportInProgress = false;
    std::unique_ptr<juce::FileChooser> fileChooser;   // 非モーダル。寿命を持たせる必要がある。

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace kvc
