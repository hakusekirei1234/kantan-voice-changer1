// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace kvc
{

//==============================================================================
/** 一時 WAV への録音。

    保存先は %LOCALAPPDATA%\SimpleVoiceChanger\rec_current.wav。
    D:\SynologyDrive 配下には絶対に書かない（同期クライアントが書き込み中の
    ファイルハンドルを掴んで録音を壊す）。MP3 化は書き出し時のみ。

    ★録音する信号
      DSP 後・ミュート前・ビープ前。つまり「相手に聞こえるはずの音」がそのまま入る。
      ミュートしても録音に穴は開かないし、ピコ音も入らない。

    ★オーディオスレッドはロックを一切取らない
      JUCE の録音デモは audioDeviceIOCallback の中で CriticalSection を取るが、
      あれは規律違反なので真似しない。停止はメッセージスレッド側で
      activeWriter を null 化 → エポック比較 → Timer で遅延解放する。

    スレッド契約:
      prepare / start / stop / discard / ファイル系 ... メッセージスレッド専用
      writeBlock                                     ... オーディオスレッド専用
      isRecording / didOverrun / getRecordedSeconds  ... どちらからでも（atomic）
*/
class Recorder : private juce::Timer
{
public:
    /** ThreadedWriter のバッファ長。同期クライアントの CPU スパイクを吸収するため
        デモの既定より大きく取る。 */
    static constexpr double kWriterBufferSeconds = 2.0;

    Recorder();
    ~Recorder() override;

    /** 録音していない間に呼ぶ。ディスクスレッドを起こす。 */
    void prepare (double sampleRate, int numChannels);

    /** 前のテイクを削除して録音を始める。失敗したら false。 */
    bool startRecording();

    /** ライタを外して遅延解放をスケジュールする。すぐには delete しない。 */
    void stopRecording();

    /** 録り直し／破棄。ファイルも消す。 */
    void discardTake();

    bool   isRecording() const noexcept { return recording.load (std::memory_order_relaxed); }
    bool   hasTake() const noexcept;
    double getRecordedSeconds() const noexcept;

    juce::File getTakeFile() const { return takeFile; }
    juce::File getRecordingFolder() const;

    /** ディスクが追いつかなかった。UI は「録音が追いつきませんでした」を出す。 */
    bool didOverrun() const noexcept { return overrun.load (std::memory_order_relaxed); }
    void clearOverrunFlag() noexcept { overrun.store (false, std::memory_order_relaxed); }

    //==========================================================================
    /** オーディオスレッド専用。録音していないときは何もしない。
        AudioEngine のマスターコールバックから、送信リングへ書く直前に呼ぶこと。 */
    void writeBlock (const float* const* channels, int numChannels, int numSamples) noexcept;

private:
    void timerCallback() override;

    juce::TimeSliceThread diskThread { "kvc-disk" };
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;

    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };
    std::atomic<uint64_t> blocksWritten { 0 };
    std::atomic<uint64_t> samplesWritten { 0 };
    std::atomic<bool>     recording { false };
    std::atomic<bool>     overrun { false };

    uint64_t retireAtBlock = 0;
    juce::int64 retireStartedMs = 0;

    juce::File takeFile;
    double sampleRate = 48000.0;
    int    numChannels = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Recorder)
};

} // namespace kvc
