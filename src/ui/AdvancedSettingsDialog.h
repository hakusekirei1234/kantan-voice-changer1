// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/Settings.h"
#include "audio/AudioEngine.h"
#include "core/Parameters.h"
#include "platform/GlobalHotkey_Win32.h"
#include "platform/RevealInExplorer.h"
#include "ui/HotkeyCaptureDialog.h"

#include <functional>

namespace kvc
{

//==============================================================================
/** 詳細設定モーダル（560x620）。タブ: オーディオ / キー設定 / 音質 / 通知音 / 書き出し / 情報。

    オーディオ … バッファサイズ（64/128/256/512/1024、「128 サンプル（約 2.7 ms）」表記）、
                 サンプルレート（自動/44100/48000）、低遅延優先↔安定優先、
                 モニター音量 / 送信音量、ASIO を使う（警告付き）、
                 排他モードを候補に出す（警告付き）、
                 「ミュート中は自分の声も止める」（既定 OFF）。

    キー設定  … HotkeyCaptureDialog を開くだけ。

    音質      … ノイズ除去の品質 低遅延(N=256, +5.3ms) / 高音質(N=512, +10.7ms)、
                 ノイズ除去の強さ 弱/標準/強。
                 ★品質を変えるとアルゴリズム遅延が変わるので、
                   適用時に必ず AudioEngine::restartAsync() を呼ぶこと。
                   実行中に遅延が変わってはならない。

    通知音    … ミュート切替時に音を鳴らす（既定 ON）、音量。
                 注記「この音はあなたにだけ聞こえ、相手には送られません。」

    書き出し  … 形式 MP3/WAV、MP3 の音質 128/192/320 kbps（既定 192）、保存先フォルダ。
                 Mp3Exporter::probeMp3Available() が false なら MP3 を選べなくし、
                 Media Feature Pack の案内を 1 行出す。

    情報      … ★現地デバッグの唯一の手段。手を抜かない。
                 ずれ補正 ppm（モニター / 送信）、音の途切れ回数、
                 実測レイテンシ内訳（デバイス入力 + ブロック + DSP + targetFill +
                 ブロック + デバイス出力）、実際に開けたレートとブロック長、
                 バックエンド名、バージョン、ログフォルダを開く、設定をリセット。
                 1 秒 Timer で更新する。

    JUCE_MODAL_LOOPS_PERMITTED=0 なので launchAsync のみ。

    スレッド契約: 全てメッセージスレッド専用。診断値は atomic 読みなので安全。
*/
class AdvancedSettingsDialog final : public juce::Component,
                                     private juce::Timer
{
public:
    AdvancedSettingsDialog (Settings&, AudioEngine&, VoiceParams&, GlobalHotkey&);
    ~AdvancedSettingsDialog() override;

    /** onClosed はメッセージスレッドで呼ばれる。呼び出し側はここで
        UI の再同期（遅延表示など）を行う。 */
    static void launchAsync (juce::Component* parentForCentring,
                             Settings&, AudioEngine&, VoiceParams&, GlobalHotkey&,
                             std::function<void()> onClosed);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;   // 情報タブの診断更新（1 Hz）
    void refreshInfoTab();

    Settings&     settings;
    AudioEngine&  engine;
    VoiceParams&  params;
    GlobalHotkey& hotkey;

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::TextButton closeButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedSettingsDialog)
};

} // namespace kvc
