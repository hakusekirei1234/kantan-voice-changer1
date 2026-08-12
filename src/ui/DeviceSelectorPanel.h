// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "audio/DeviceCatalog.h"

#include <functional>

namespace kvc
{

//==============================================================================
/** 「① 機器を選ぶ」パネル。3 列 = 入力（マイク）／自分に聞こえる音／相手に送る音。

    - 1 エンドポイント 1 行の統合リスト。バッジ [低遅延]/[通常]/[排他]/[ASIO] を付ける。
      同じエンドポイントを 3 つの WASAPI タイプが重複列挙するので、生の
      getDeviceNames() を素直に並べてはいけない。
    - 排他モードと ASIO の行は詳細設定を ON にしたときだけ出す。どちらも
      SYNCROOM との共存を壊す。
    - 「相手に送る音」は送信先候補（SYNCROOM Driver / VoiceMeeter / CABLE）を
      先頭にピン留めし、「送信しない（モニターのみ）」を第一級の選択肢として置く。
    - 入力==送信、モニター==送信、入力が送信先のループバック側、の 3 つは
      選ばせない（グレーアウト + ツールチップ）。事後にエラーを出すのでは遅い。
      非技術者が間違えると全音量のハウリングがヘッドホンに返る。
    - 入力とモニターが同一なら「✓ 入力とモニターが同じ機器なので、いちばん
      遅れが少ない状態です。」を出す。これが推奨構成。

    スレッド契約: 全メソッドがメッセージスレッド専用。
*/
class DeviceSelectorPanel final : public juce::Component,
                                  private DeviceCatalog::Listener
{
public:
    enum class Column
    {
        input = 0,
        monitor = 1,
        send = 2
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        /** ユーザーがどれかの列を変更した。呼ばれた側はエンジンを作り直す。 */
        virtual void deviceSelectionChanged() = 0;
        /** 「機器を再検索」が押された。 */
        virtual void deviceRescanRequested() = 0;
    };

    explicit DeviceSelectorPanel (DeviceCatalog&);
    ~DeviceSelectorPanel() override;

    /** 保存済みのエンドポイント ID を反映する。見つからない ID は
        選択なしにして、赤い注意文を出す（黙って別の機器に差し替えない）。 */
    void setSelection (const juce::String& inputId,
                       const juce::String& monitorId,
                       const juce::String& sendId);

    juce::String getInputEndpointId() const;
    juce::String getMonitorEndpointId() const;
    juce::String getSendEndpointId() const;    ///< 「送信しない」なら空文字

    /** 各列の下の 11px グレー行。例: 「48000 Hz / 128 サンプル / 接続中」。
        isError なら赤字にする。 */
    void setStatusLine (Column, const juce::String& text, bool isError);

    /** 排他モード / ASIO の行を出すか。詳細設定と連動する。 */
    void setAdvancedBackendsVisible (bool);

    /** カタログを読み直して ComboBox を作り直す。選択は可能な限り維持する。 */
    void refresh();

    void addListener (Listener*);
    void removeListener (Listener*);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void deviceListChanged() override;   // DeviceCatalog::Listener

    DeviceCatalog& catalog;
    juce::ListenerList<Listener> listeners;

    juce::Label  sectionLabel;
    juce::TextButton rescanButton;
    juce::Label  columnLabels[3];
    juce::ComboBox combos[3];
    juce::Label  statusLines[3];
    juce::Label  hintLabel;      ///< 「[低遅延]」と付いた機器を選ぶと遅れが少なくなります
    juce::Label  warningLabel;   ///< ハウリング等の赤い警告

    bool advancedVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSelectorPanel)
};

} // namespace kvc
