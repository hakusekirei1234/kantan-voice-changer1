// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

namespace kvc
{

//==============================================================================
/** Media Foundation による MP3 書き出し。LAME は同梱しない。

    重要な実装上の約束（mp3.json 準拠。どれも外すと静かに壊れる）:
      - 専用の MTA スレッドで CoInitializeEx(COINIT_MULTITHREADED) →
        MFStartup(MF_VERSION, MFSTARTUP_LITE) → ... → MFShutdown → CoUninitialize。
        JUCE のメッセージスレッドは STA なので MF インターフェースを渡さないこと。
      - パスは URL として渡さず、MFCreateFile でワイド文字パスから IMFByteStream を作る。
      - 属性に MF_TRANSCODE_CONTAINERTYPE = MFTranscodeContainerType_MP3 を必ず入れる
        （バイトストリーム形式では必須）。MF_SINK_WRITER_DISABLE_THROTTLING = TRUE も。
      - 出力型を先、入力型を後（AddStream → SetInputMediaType の順）。
      - 入力は 16 bit 整数 PCM のみ。float は受け付けない。クランプしてから変換する。
      - MF_MT_AUDIO_AVG_BYTES_PER_SECOND は「バイト毎秒」。192 kbps = 24000。
        192000 を渡すのが最頻出のバグ。
      - IMFMediaBuffer::SetCurrentLength は既定 0。忘れると無音の MP3 ができる。
      - Finalize() を忘れると途中で切れた再生不能ファイルになる。エラー経路でも呼ぶ。
      - サンプルレートは 48000 / 44100 / 32000 のみ。88.2k / 96k / 192k は事前に自前で
        48000 へリサンプルする（シンクライタの暗黙リサンプラを当てにしない）。
      - 一時ファイルへ書いてから MoveFileExW(MOVEFILE_REPLACE_EXISTING) で本配置。
        同期フォルダに書きかけの mp3 を置かないため。

    Windows N 版対策: mf.dll / mfplat.dll / mfreadwrite.dll は /DELAYLOAD しているので、
    MP3 が無い環境でも exe は起動する。UI に MP3 を出す前に probeMp3Available() を通すこと。

    スレッド契約:
      probeMp3Available ... 任意のスレッド。初回だけ実測し、以後キャッシュを返す。
                            メッセージスレッドから呼ぶと数十 ms 止まる可能性がある。
      exportBlocking    ... 専用のバックグラウンドスレッド専用。MTA の初期化も内部で行う。
      exportAsync       ... メッセージスレッドから呼び、完了通知もメッセージスレッドで受ける。
*/
class Mp3Exporter
{
public:
    enum class Format
    {
        mp3 = 0,
        wav = 1
    };

    enum class Result
    {
        success = 0,
        successWithWavFallback,   ///< MP3 に失敗したので WAV で保存した
        mp3Unavailable,           ///< N 版 / セーフモード。UI では MP3 を選べなくする。
        sourceMissing,
        encodeFailed,
        destinationFailed
    };

    struct Request
    {
        juce::File sourceWav;         ///< Recorder が書いた一時 WAV
        juce::File destination;       ///< 拡張子込みの最終パス
        Format     format = Format::mp3;
        int        bitrateKbps = 192; ///< 128 / 192 / 320
    };

    struct Outcome
    {
        Result       result = Result::encodeFailed;
        juce::File   writtenFile;
        juce::String japaneseMessage;   ///< そのまま UI に出せる
    };

    /** MP3 エンコーダの有無。MFTranscodeGetAudioOutputAvailableTypes が
        非空のコレクションを返すかで判定する。結果はプロセス内でキャッシュする。 */
    static bool probeMp3Available();

    /** 同期実行。専用スレッドから呼ぶこと。 */
    static Outcome exportBlocking (const Request&);

    /** バックグラウンドスレッドを起こして実行し、完了時に onFinished を
        メッセージスレッドで呼ぶ。JUCE_MODAL_LOOPS_PERMITTED=0 なので
        ThreadWithProgressWindow::runThread() は使えない。進捗表示が要る場合は
        呼び出し側が非モーダルのダイアログを出し、onFinished で閉じること。 */
    static void exportAsync (const Request&, std::function<void (Outcome)> onFinished);

    /** 保存ダイアログの既定ファイル名: ボイチェン_YYYYMMDD_HHMM.mp3 */
    static juce::String makeDefaultFileName (Format);

    /** 既定の保存先 %USERPROFILE%\Music\簡単ボイチェン。無ければ作る。
        同期フォルダを既定にしてはいけない。 */
    static juce::File getDefaultExportFolder();

private:
    Mp3Exporter() = delete;
};

} // namespace kvc
