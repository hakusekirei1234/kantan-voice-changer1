// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>

namespace kvc
{

//==============================================================================
/** ミュート切替の通知音。起動時に一度だけ合成する 60 ms の短音。

    ミュート ON  = 1200 Hz、ミュート OFF = 800 Hz。
    どちらも 5 ms のレイズドコサインで立ち上げ・立ち下げるのでクリックしない。
    音声ファイル資産は使わない（合成のみ）。

    ★構造的な保証: このクラスの出力はモニターバスにしか足されない。
      送信バスのコールバックにはビープのコードが存在しない。条件分岐ではなく
      「コードが無い」ことで相手に届かないことを保証する。録音タップもビープより
      上流にあるので、書き出した MP3 にもピコ音は入らない。

    スレッド契約:
      prepare        ... メッセージスレッド専用（確保する）
      trigger        ... メッセージスレッド専用（ホットキーハンドラから）
      setVolume / setEnabled ... どのスレッドからでも
      addToMonitor   ... モニターを鳴らすオーディオコールバック 1 本からのみ。
                         collapse 構成ではマスターコールバック、別デバイス構成では
                         モニタースレーブのコールバック。同時に 2 箇所から呼ばないこと
                         （再生位置がスレッド専有の生値だから）。
      resetPlayback  ... デバイス停止中のみ
*/
class BeepGenerator
{
public:
    enum class Kind
    {
        muteOn = 0,   ///< 1200 Hz
        muteOff = 1   ///< 800 Hz
    };

    static constexpr double kDurationSeconds = 0.060;
    static constexpr double kRampSeconds = 0.005;
    static constexpr double kFreqMuteOn = 1200.0;
    static constexpr double kFreqMuteOff = 800.0;

    BeepGenerator();
    ~BeepGenerator();

    /** 2 種類の波形を合成して確保する。サンプルレート変更のたびに呼び直す。 */
    void prepare (double sampleRate);

    void setVolume (float linearGain) noexcept;
    void setEnabled (bool shouldBeEnabled) noexcept;

    /** シーケンス番号を進めるだけ。連打しても取りこぼさず、鳴り始めから
        きれいに鳴り直す。オーディオ側は seq の変化だけを見る。 */
    void trigger (Kind kind) noexcept;

    /** モニターバスに加算する。無音時は何もしない（確保も分岐コストも無い）。 */
    void addToMonitor (float* const* dest, int numDestChannels, int numSamples) noexcept;

    /** 再生中の音を切って先頭に戻す。デバイス停止中のみ。 */
    void resetPlayback() noexcept;

    bool isPrepared() const noexcept { return prepared; }

private:
    juce::AudioBuffer<float> waveforms;   // 2 行 (muteOn / muteOff)、同じ長さ
    int  lengthSamples = 0;
    bool prepared = false;

    std::atomic<uint32_t> seq { 0 };
    std::atomic<int>      kindRequested { 0 };
    std::atomic<float>    volume { 0.6f };
    std::atomic<bool>     enabled { true };

    // モニターコールバック専有
    uint32_t lastSeqSeen = 0;
    int      playPos = -1;     // -1 = 停止中
    int      playIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeepGenerator)
};

} // namespace kvc
