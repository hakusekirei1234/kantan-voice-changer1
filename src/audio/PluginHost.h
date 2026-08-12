// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "core/ExternalProcessor.h"

namespace kvc
{

//==============================================================================
/** 読み込み済みプラグイン。ExternalProcessor に GUI と状態保存を足したもの。 */
class LoadedPlugin : public ExternalProcessor
{
public:
    ~LoadedPlugin() override = default;

    virtual bool hasEditor() const = 0;

    /** プラグイン自身の画面。所有権は呼び出し側。メッセージスレッド専用。 */
    virtual juce::Component* createEditor() = 0;

    /** メッセージスレッド専用。 */
    virtual juce::MemoryBlock saveState() = 0;
    virtual void restoreState (const juce::MemoryBlock&) = 0;

    /** 名前からピッチ／フォルマント相当のパラメータを推測して割り当てる。
        見つからなければ -1。プラグイン画面を開かずに操作させるために使う。 */
    virtual int findParameterIndexByKeywords (const juce::StringArray& keywords) const = 0;
    virtual void setParameterNormalised (int index, float value) = 0;
    virtual float getParameterNormalised (int index) const = 0;
    virtual juce::String getParameterText (int index) const = 0;
};

//==============================================================================
/** プラグインの探索と読み込み。メッセージスレッド専用。

    VST3 は JUCE の VST3PluginFormat をそのまま使う（SDK は JUCE 同梱、ライセンス手続き不要）。

    VST2 は JUCE を使わない。JUCE の VST2 ホストは Steinberg の VST2 SDK ヘッダを
    要求するが JUCE はそれを同梱しておらず、Steinberg は新規ライセンスを出していない。
    そこで ABI 定義を自前で持つ最小ホスト（Vst2Host.cpp）を使い、
    フォルダ走査もここで独自に行う。KVC_VST2_ENABLED=0 のビルドでは丸ごと消える。

    ★プラグインは当アプリと同じプロセスで動く。クラッシュするプラグインはアプリごと
      落とすので、読み込み前に「これから開く」印をディスクに残し、次回起動時に
      その印が残っていたら当該プラグインを自動で除外する。
*/
class PluginHost
{
public:
    struct Entry
    {
        juce::String identifier;    ///< 保存用の一意キー。"VST3:..." / "VST2:<path>"
        juce::String name;
        juce::String manufacturer;
        juce::String format;        ///< "VST3" / "VST2"
        juce::String path;
    };

    explicit PluginHost (const juce::File& stateFolder);
    ~PluginHost();

    /** 標準フォルダを走査する。数秒かかることがある。 */
    void scan();

    /** 走査に出てこないプラグインを直接指定する。 */
    bool addFromFile (const juce::File&, juce::String& errorJapanese);

    juce::Array<Entry> getEntries() const { return entries; }
    const Entry* findEntry (const juce::String& identifier) const;

    /** 失敗したら nullptr を返し errorJapanese を埋める。 */
    std::unique_ptr<LoadedPlugin> load (const juce::String& identifier,
                                        double sampleRate, int maxBlockSize,
                                        juce::String& errorJapanese);

    /** 前回の起動で落ちたプラグインの identifier 一覧。 */
    juce::StringArray getBlockedIdentifiers() const { return blocked; }
    void unblock (const juce::String& identifier);

    static juce::StringArray pitchKeywords();
    static juce::StringArray formantKeywords();

private:
    void loadPersistedState();
    void savePersistedState() const;

    /** 「これから開く」印。開き終えたら消す。残っていたら前回落ちている。 */
    void markOpening (const juce::String& identifier);
    void clearOpeningMark();
    juce::String readOpeningMark() const;

    void scanVst3();
    void scanVst2();

    juce::File folder;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownList;

    juce::Array<Entry> entries;
    juce::StringArray  blocked;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace kvc
