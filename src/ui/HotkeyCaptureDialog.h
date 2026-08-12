// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "platform/GlobalHotkey_Win32.h"

#include <functional>

namespace kvc
{

//==============================================================================
/** ミュートキーの割当ダイアログ。

    構成:
      現在:  [ Ctrl+Shift+M ]        [ 変更 ]
      「安全なキー」から選ぶ ▾  ( 無変換 / 変換 / Pause / F9 / F12 / Ctrl+Shift+M )
      動作   (•) 押すたびに切り替え（トグル）
             ( ) 押している間だけミュート
      [✓] 他のアプリでもこのキーを使えるようにする（推奨）

    キャプチャの手順:
      1. 本番のバインドを uninstall する（キャプチャ中に古いキーが発火しないように）。
      2. HotkeyCapture で一時 LL フックを張る。Esc 以外は消費して他アプリへ漏らさない。
      3. Enter / Space 単独は「このキーは設定できません」で拒否する。
      4. 修飾なしの文字・数字キーが取れたら、その場で赤字の警告を出す:
         「⚠「V」のように文字キーだけを設定すると、ほかのアプリで文字を入力する
          たびにミュートが切り替わってしまいます。Ctrl や Shift と組み合わせるか、
          「無変換」キーや F9 などをおすすめします。」
         ボタンは [このまま設定する] / [おすすめのキーにする]。
      5. install が ERROR_HOTKEY_ALREADY_REGISTERED で失敗したら
         「このキーは他のアプリが使用中です。別のキーを選んでください。」を出して
         ダイアログを閉じない。
      6. 割当後に「押してみてください」を出し、実際に発火したら緑の ✓ を点ける。
         非技術者は動くところを見ないと壊れていると思い込む。

    JUCE_MODAL_LOOPS_PERMITTED=0 なので showModal 系は使えない。
    必ず DialogWindow::LaunchOptions::launchAsync() で出し、結果はコールバックで返す。

    スレッド契約: 全てメッセージスレッド専用。
*/
class HotkeyCaptureDialog final : public juce::Component,
                                  private HotkeyCapture::Listener,
                                  private GlobalHotkey::Listener
{
public:
    /** accepted が false ならユーザーがキャンセルした。binding は変更しない。 */
    using CompletionCallback = std::function<void (bool accepted, HotkeyBinding binding)>;

    HotkeyCaptureDialog (GlobalHotkey&, HotkeyBinding current, CompletionCallback);
    ~HotkeyCaptureDialog() override;

    /** 非モーダルで開く。dialog はウィンドウに所有される。 */
    static void launchAsync (juce::Component* parentForCentring,
                             GlobalHotkey&,
                             HotkeyBinding current,
                             CompletionCallback);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void keyCaptured (uint32_t vk, uint32_t mods) override;
    void captureCancelled() override;

    /** GlobalHotkey::Listener。手順 6 の「押してみてください」で、割り当てた
        キーが本当に発火したことを知る手段はこれしか無い。GetAsyncKeyState を
        ポーリングしても「キーが押された」しか分からず、RegisterHotKey が
        黙って失敗していても緑の ✓ が点いてしまう。 */
    void hotkeyPressed() override;

    void refreshUi();
    void beginCapture();
    void endCapture (const juce::String& statusJapanese);
    void restoreLiveBinding();
    void applyAndTest();
    void finish (bool accepted);

    GlobalHotkey&      hotkey;
    HotkeyCapture      capture;
    HotkeyBinding      original;    ///< ダイアログを開いた時点のバインド。取り消し時の復帰先。
    HotkeyBinding      working;
    CompletionCallback onComplete;

    bool applied = false;      ///< working の install に成功して現在有効
    bool confirmed = false;    ///< 実際に発火するのを目視確認できた
    int  captureSecondsLeft = 0;

    juce::Label      currentLabel;
    juce::TextButton changeButton;
    juce::ComboBox   safeKeyCombo;
    juce::ToggleButton modeToggle, pushToMuteToggle, passThroughToggle;
    juce::Label      warningLabel;
    juce::Label      confirmLabel;      ///< 「押してみてください」→ 緑の ✓
    juce::TextButton okButton, cancelButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotkeyCaptureDialog)
};

} // namespace kvc
