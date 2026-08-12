// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "audio/Recorder.h"

#include "core/Parameters.h"

namespace kvc
{

//==============================================================================
namespace
{
    /** 退避待ちの上限。これを超えたらコールバックはもう来ていないと判断する。
        マスターコールバックが 1 秒以上戻ってこない状況は既にデバイス側の故障で、
        そのときライタを抱えたままにするとアプリ終了がハングする。 */
    constexpr juce::int64 kRetireTimeoutMs = 1000;

    /** 一時 WAV のビット深度。24 bit にしておくと「WAV で書き出す」を選んだ人が
        そのまま DAW に持ち込める。MP3 に落とすときだけ 16 bit へ変換する。 */
    constexpr int kTakeBitsPerSample = 24;

    constexpr const char* kTakeFileName = "rec_current.wav";
}

//==============================================================================
Recorder::Recorder()
{
    takeFile = getRecordingFolder().getChildFile (kTakeFileName);
}

Recorder::~Recorder()
{
    stopRecording();
    stopTimer();

    // ThreadedWriter を先に壊す。デストラクタが残りをフラッシュして
    // WAV ヘッダを確定させるので、ディスクスレッドはそのあとで止める。
    writer.reset();
    activeWriter.store (nullptr, std::memory_order_release);

    diskThread.stopThread (2000);
}

//==============================================================================
juce::File Recorder::getRecordingFolder() const
{
    // %LOCALAPPDATA%。ユーザーのドキュメント配下は Synology Drive の同期対象に
    // なっていることがあり、書き込み中のファイルを掴まれて録音が壊れる。
    auto folder = juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
                      .getChildFile ("SimpleVoiceChanger");

    if (! folder.isDirectory())
        folder.createDirectory();

    return folder;
}

void Recorder::prepare (double newSampleRate, int newNumChannels)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());
    jassert (! isRecording());

    sampleRate  = newSampleRate > 0.0 ? newSampleRate : kPreferredSampleRate;
    numChannels = juce::jlimit (1, 2, newNumChannels);

    takeFile = getRecordingFolder().getChildFile (kTakeFileName);

    if (! diskThread.isThreadRunning())
        diskThread.startThread (juce::Thread::Priority::normal);
}

//==============================================================================
bool Recorder::startRecording()
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    // 1 スロットしか持たない。録り直しは前テイクの破棄から始まる。
    discardTake();

    auto folder = getRecordingFolder();

    if (! folder.isDirectory())
        return false;

    takeFile = folder.getChildFile (kTakeFileName);
    takeFile.deleteFile();

    if (! diskThread.isThreadRunning())
        diskThread.startThread (juce::Thread::Priority::normal);

    auto fileStream = std::make_unique<juce::FileOutputStream> (takeFile);

    if (! fileStream->openedOk())
        return false;

    fileStream->setPosition (0);
    fileStream->truncate();

    // 成功時のみ所有権がライタへ移る（失敗しても stream は生き残るので二重解放にならない）。
    std::unique_ptr<juce::OutputStream> stream (std::move (fileStream));

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (kTakeBitsPerSample);

    auto fileWriter = wavFormat.createWriterFor (stream, options);

    if (fileWriter == nullptr)
        return false;

    const int bufferSamples = juce::jmax (8192, (int) (kWriterBufferSeconds * sampleRate));

    writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (fileWriter.release(),
                                                                       diskThread,
                                                                       bufferSamples);

    samplesWritten.store (0, std::memory_order_relaxed);
    overrun.store (false, std::memory_order_relaxed);
    recording.store (true, std::memory_order_relaxed);

    // release ストア。オーディオスレッドがこのポインタを acquire で観測した時点で
    // sampleRate / numChannels / ライタ本体の初期化はすべて可視になっている。
    activeWriter.store (writer.get(), std::memory_order_release);

    return true;
}

void Recorder::stopRecording()
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    recording.store (false, std::memory_order_relaxed);

    if (activeWriter.load (std::memory_order_relaxed) == nullptr && writer == nullptr)
        return;

    activeWriter.store (nullptr, std::memory_order_release);

    // ここで delete してはいけない。null 化の直前にポインタを読んだコールバックが
    // まだ write() の中にいる可能性がある。1 デバイスのコールバックは直列なので、
    // 「null 化より後にコールバックが 1 回完走した」ことを観測できれば安全になる。
    retireAtBlock   = blocksWritten.load (std::memory_order_acquire) + 2;
    retireStartedMs = juce::Time::currentTimeMillis();

    startTimer (25);
}

void Recorder::timerCallback()
{
    if (writer == nullptr)
    {
        stopTimer();
        return;
    }

    const bool drained  = blocksWritten.load (std::memory_order_acquire) >= retireAtBlock;
    const bool timedOut = (juce::Time::currentTimeMillis() - retireStartedMs) > kRetireTimeoutMs;

    if (drained || timedOut)
    {
        writer.reset();     // ここで残りがフラッシュされ WAV ヘッダが確定する
        stopTimer();
    }
}

void Recorder::discardTake()
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    stopRecording();

    // ファイルを消す前にライタを確実に閉じる。ユーザー操作起点なので
    // メッセージスレッドを最大 500ms だけ待たせるのは許容する。
    const auto deadline = juce::Time::currentTimeMillis() + 500;

    while (writer != nullptr
           && blocksWritten.load (std::memory_order_acquire) < retireAtBlock
           && juce::Time::currentTimeMillis() < deadline)
    {
        juce::Thread::sleep (5);
    }

    stopTimer();
    writer.reset();

    samplesWritten.store (0, std::memory_order_relaxed);
    overrun.store (false, std::memory_order_relaxed);

    takeFile.deleteFile();
}

//==============================================================================
bool Recorder::hasTake() const noexcept
{
    // writer が生きている間はまだ WAV ヘッダが確定していないので「テイクあり」にしない。
    return writer == nullptr
           && samplesWritten.load (std::memory_order_relaxed) > 0
           && takeFile.existsAsFile();
}

double Recorder::getRecordedSeconds() const noexcept
{
    const auto n = samplesWritten.load (std::memory_order_relaxed);
    return sampleRate > 0.0 ? (double) n / sampleRate : 0.0;
}

//==============================================================================
void Recorder::writeBlock (const float* const* channels, int numSourceChannels, int numSamples) noexcept
{
    auto* w = activeWriter.load (std::memory_order_acquire);

    if (w != nullptr
        && channels != nullptr
        && numSamples > 0
        && numSourceChannels >= numChannels)
    {
        // ThreadedWriter::write はロックを取らず、内部の AbstractFifo に積むだけ。
        // 追いつかなければ false が返る（ブロックはしない）。
        if (! w->write (channels, numSamples))
            overrun.store (true, std::memory_order_relaxed);

        samplesWritten.fetch_add ((uint64_t) numSamples, std::memory_order_relaxed);
    }

    // 退避判定用のエポック。ライタが外れていても必ず進める。進めないと停止側が
    // 「コールバックが 1 回完走した」ことを観測できず、常にタイムアウト頼みになる。
    blocksWritten.fetch_add (1, std::memory_order_release);
}

} // namespace kvc
