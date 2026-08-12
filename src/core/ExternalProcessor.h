// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_core/juce_core.h>

namespace kvc
{

//==============================================================================
/** 外部プラグインを VoiceEngine の中に差し込むための最小インターフェース。

    VST3（JUCE 経由）と VST2（自前ホスト）の両方がこれを実装する。
    ここには GUI を一切持ち込まない。kvc_core を将来 VST3 化するときに
    juce_gui_basics へ依存させないため。エディタや状態保存は PluginHost 側の
    LoadedPlugin が持つ。

    スレッド契約:
      prepare / reset    ... メッセージスレッド専用（確保してよい）
      processMono        ... オーディオスレッド専用（確保・ロック・例外いっさい禁止）
      getLatencySamples  ... prepare 後は不変。どのスレッドからでも。

    ★遅延は prepare 時に確定していなければならない。VoiceEngine の遅延計算は
      「実行中は不変」を前提にしており、送信とモニターの遅延補正がここに乗るため。
      プラグインの差し替えは必ずエンジンを止めてから行う。
*/
class ExternalProcessor
{
public:
    virtual ~ExternalProcessor() = default;

    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() = 0;

    /** in-place。numSamples <= prepare で渡した maxBlockSize。 */
    virtual void processMono (float* buffer, int numSamples) noexcept = 0;

    virtual int getLatencySamples() const noexcept = 0;

    /** 画面表示用の名前。 */
    virtual juce::String getName() const = 0;
};

} // namespace kvc
