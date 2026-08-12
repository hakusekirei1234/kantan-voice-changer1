// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "audio/Mp3Exporter.h"

#include <juce_events/juce_events.h>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmreg.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>


namespace
{
    // JUCE の String(const char*) は ASCII 専用（CharPointer_ASCII）。
    // UTF-8 の日本語リテラルは必ずこれを通す。通さないと 1 バイトずつ別文字に化ける。
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

namespace kvc
{

//==============================================================================
namespace
{
    constexpr int kChunkFrames = 4096;

    /** MPEG-1 のビットレート表。これ以外を渡すとエンコーダが型を拒否する。 */
    constexpr int kAllowedBitratesKbps[] =
        { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };

    /** MP3 判定のプロセス内キャッシュ。-1=未測定 / 0=不可 / 1=可。 */
    std::atomic<int> mp3ProbeCache { -1 };

    //==========================================================================
    template <typename T>
    class ComPtr
    {
    public:
        ComPtr() = default;
        ~ComPtr() { reset(); }

        ComPtr (const ComPtr&) = delete;
        ComPtr& operator= (const ComPtr&) = delete;

        T** put() noexcept          { reset(); return &ptr; }
        T*  get() const noexcept    { return ptr; }
        T*  operator->() const noexcept { return ptr; }
        explicit operator bool() const noexcept { return ptr != nullptr; }

        void reset() noexcept
        {
            if (ptr != nullptr)
            {
                ptr->Release();
                ptr = nullptr;
            }
        }

    private:
        T* ptr = nullptr;
    };

    //==========================================================================
    /** mf.dll / mfplat.dll / mfreadwrite.dll は /DELAYLOAD している。Windows N 版のように
        存在しない環境で最初の API を呼ぶと SEH 例外になり、C++ の try では捕まえられない。
        そこで呼ぶ前に必ずここを通す。一度ロードしたら FreeLibrary しない
        （遅延ロードのサンクが解放済みモジュールを指したままになるため）。 */
    bool mediaFoundationDllsPresent()
    {
        static const bool present = []
        {
            const wchar_t* const names[] = { L"mfplat.dll", L"mfreadwrite.dll", L"mf.dll" };

            for (auto* n : names)
                if (LoadLibraryW (n) == nullptr)
                    return false;

            return true;
        }();

        return present;
    }

    /** MTA + MFStartup の対。exportBlocking / プローブスレッドの入口で 1 回だけ作る。 */
    struct MfSession
    {
        MfSession()
        {
            // 既に STA として初期化済みのスレッドなら RPC_E_CHANGED_MODE。
            // その場合 CoUninitialize してはいけない（他人の初期化を壊す）。
            comInitialised = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
            mfStarted      = SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE));
        }

        ~MfSession()
        {
            if (mfStarted)
                MFShutdown();

            if (comInitialised)
                CoUninitialize();
        }

        MfSession (const MfSession&) = delete;
        MfSession& operator= (const MfSession&) = delete;

        bool ok() const noexcept { return mfStarted; }

        bool comInitialised = false;
        bool mfStarted = false;
    };

    DWORD encoderEnumFlags() noexcept
    {
        return (DWORD) ((MFT_ENUM_FLAG_ALL & ~(DWORD) MFT_ENUM_FLAG_FIELDOFUSE)
                        | (DWORD) MFT_ENUM_FLAG_SORTANDFILTER);
    }

    /** MF が起動済みのスレッドから呼ぶこと。 */
    bool probeOnThisThread()
    {
        ComPtr<IMFCollection> types;

        if (FAILED (MFTranscodeGetAudioOutputAvailableTypes (MFAudioFormat_MP3,
                                                             encoderEnumFlags(),
                                                             nullptr,
                                                             types.put())))
            return false;

        if (! types)
            return false;

        DWORD count = 0;

        if (FAILED (types->GetElementCount (&count)))
            return false;

        return count > 0;
    }

    //==========================================================================
    int clampBitrateKbps (int requested) noexcept
    {
        int best = 192;
        int bestDistance = INT_MAX;

        for (int k : kAllowedBitratesKbps)
        {
            const int d = std::abs (k - requested);

            if (d < bestDistance)
            {
                bestDistance = d;
                best = k;
            }
        }

        return best;
    }

    /** MPEG-1 として合法なレート。88.2k / 96k / 192k はここに入らないので事前リサンプル。 */
    bool isEncoderSampleRate (double sr) noexcept
    {
        const int r = (int) std::lround (sr);
        return r == 48000 || r == 44100 || r == 32000;
    }

    bool moveIntoPlace (const juce::File& from, const juce::File& to)
    {
        // juce::String::toWideCharPointer が返すのは String 内部の一時領域なので、
        // 名前付きの String を生かしたまま使うこと。
        const juce::String fromPath = from.getFullPathName();
        const juce::String toPath   = to.getFullPathName();

        return MoveFileExW (fromPath.toWideCharPointer(),
                            toPath.toWideCharPointer(),
                            MOVEFILE_REPLACE_EXISTING) != 0;
    }

    //==========================================================================
    /** 列挙で出力型を得る（MF_MT_USER_DATA まで完成した型が手に入る）。
        目標ビットレート以下で最大のもの、無ければ最小の上位を選ぶ。 */
    bool buildOutputTypeByEnumeration (int channels, int sampleRate, int avgBytesPerSecond,
                                       ComPtr<IMFMediaType>& result)
    {
        ComPtr<IMFCollection> types;

        if (FAILED (MFTranscodeGetAudioOutputAvailableTypes (MFAudioFormat_MP3,
                                                             encoderEnumFlags(),
                                                             nullptr,
                                                             types.put()))
            || ! types)
            return false;

        DWORD count = 0;

        if (FAILED (types->GetElementCount (&count)) || count == 0)
            return false;

        DWORD bestBelowIndex = count, bestAboveIndex = count;
        UINT32 bestBelowBytes = 0, bestAboveBytes = 0;

        for (DWORD i = 0; i < count; ++i)
        {
            ComPtr<IUnknown> element;

            if (FAILED (types->GetElement (i, element.put())) || ! element)
                continue;

            ComPtr<IMFMediaType> candidate;

            // IID_PPV_ARGS はマクロ引数を 2 回展開するので put() とは併用しない。
            if (FAILED (element->QueryInterface (__uuidof (IMFMediaType), (void**) candidate.put()))
                || ! candidate)
                continue;

            UINT32 rate = 0, ch = 0, bytes = 0;

            if (FAILED (candidate->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate))
                || FAILED (candidate->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &ch))
                || FAILED (candidate->GetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes)))
                continue;

            if ((int) rate != sampleRate || (int) ch != channels)
                continue;

            if ((int) bytes <= avgBytesPerSecond)
            {
                if (bestBelowIndex == count || bytes > bestBelowBytes)
                {
                    bestBelowIndex = i;
                    bestBelowBytes = bytes;
                }
            }
            else if (bestAboveIndex == count || bytes < bestAboveBytes)
            {
                bestAboveIndex = i;
                bestAboveBytes = bytes;
            }
        }

        const DWORD chosen = bestBelowIndex != count ? bestBelowIndex : bestAboveIndex;

        if (chosen == count)
            return false;

        ComPtr<IUnknown> element;

        if (FAILED (types->GetElement (chosen, element.put())) || ! element)
            return false;

        return SUCCEEDED (element->QueryInterface (__uuidof (IMFMediaType), (void**) result.put()))
               && result;
    }

    /** 列挙が使えなかったときの決定論的な組み立て。MFInitMediaTypeFromWaveFormatEx が
        末尾 12 バイトを MF_MT_USER_DATA に詰めてくれる（エンコーダはこれを要求する）。 */
    bool buildOutputTypeByWaveFormat (int channels, int sampleRate, int avgBytesPerSecond,
                                      ComPtr<IMFMediaType>& result)
    {
        MPEGLAYER3WAVEFORMAT fmt = {};
        fmt.wfx.wFormatTag      = WAVE_FORMAT_MPEGLAYER3;
        fmt.wfx.nChannels       = (WORD) channels;
        fmt.wfx.nSamplesPerSec  = (DWORD) sampleRate;
        fmt.wfx.nAvgBytesPerSec = (DWORD) avgBytesPerSecond;
        fmt.wfx.nBlockAlign     = 1;
        fmt.wfx.wBitsPerSample  = 0;
        fmt.wfx.cbSize          = MPEGLAYER3_WFX_EXTRA_BYTES;
        fmt.wID                 = MPEGLAYER3_ID_MPEG;
        fmt.fdwFlags            = MPEGLAYER3_FLAG_PADDING_OFF;
        fmt.nBlockSize          = (WORD) ((144 * avgBytesPerSecond * 8) / sampleRate);
        fmt.nFramesPerBlock     = 1;
        fmt.nCodecDelay         = 0;

        if (FAILED (MFCreateMediaType (result.put())) || ! result)
            return false;

        return SUCCEEDED (MFInitMediaTypeFromWaveFormatEx (result.get(),
                                                           (const WAVEFORMATEX*) &fmt,
                                                           (UINT32) sizeof (fmt)));
    }

    //==========================================================================
    using ChunkFiller = std::function<void (juce::AudioBuffer<float>&, juce::int64, int)>;

    HRESULT encodeMp3 (const juce::File& outFile,
                       int channels,
                       int sampleRate,
                       int avgBytesPerSecond,
                       juce::int64 totalFrames,
                       const ChunkFiller& fillChunk)
    {
        const juce::String outPath = outFile.getFullPathName();

        // URL としてではなくバイトストリームとして渡す。非 ASCII パスを URL 解析されると壊れる。
        ComPtr<IMFByteStream> byteStream;
        HRESULT hr = MFCreateFile (MF_ACCESSMODE_WRITE,
                                   MF_OPENMODE_DELETE_IF_EXIST,
                                   MF_FILEFLAGS_NONE,
                                   outPath.toWideCharPointer(),
                                   byteStream.put());

        if (FAILED (hr))
            return hr;

        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes (attributes.put(), 2);

        if (FAILED (hr))
            return hr;

        // バイトストリーム形式では MF_TRANSCODE_CONTAINERTYPE が必須。
        attributes->SetGUID (MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MP3);
        attributes->SetUINT32 (MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

        ComPtr<IMFSinkWriter> sinkWriter;
        hr = MFCreateSinkWriterFromURL (nullptr, byteStream.get(), attributes.get(), sinkWriter.put());

        if (FAILED (hr))
            return hr;

        ComPtr<IMFMediaType> outputType;

        if (! buildOutputTypeByEnumeration (channels, sampleRate, avgBytesPerSecond, outputType))
            if (! buildOutputTypeByWaveFormat (channels, sampleRate, avgBytesPerSecond, outputType))
                return MF_E_INVALIDMEDIATYPE;

        // 出力型が先、入力型が後。逆にすると必ず失敗する。
        DWORD streamIndex = 0;
        hr = sinkWriter->AddStream (outputType.get(), &streamIndex);

        if (FAILED (hr))
            return hr;

        const int blockAlign = channels * 2;   // 16 bit 固定

        ComPtr<IMFMediaType> inputType;
        hr = MFCreateMediaType (inputType.put());

        if (FAILED (hr))
            return hr;

        inputType->SetGUID   (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inputType->SetGUID   (MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inputType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) channels);
        inputType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
        inputType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);   // 32 bit float は受け付けない
        inputType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32) blockAlign);
        inputType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (blockAlign * sampleRate));
        inputType->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        hr = sinkWriter->SetInputMediaType (streamIndex, inputType.get(), nullptr);

        if (FAILED (hr))
            return hr;

        hr = sinkWriter->BeginWriting();

        if (FAILED (hr))
            return hr;

        juce::AudioBuffer<float> chunk (channels, kChunkFrames);
        std::vector<int16_t> interleaved ((size_t) kChunkFrames * (size_t) channels);

        for (juce::int64 pos = 0; pos < totalFrames && SUCCEEDED (hr); )
        {
            const int frames = (int) juce::jmin ((juce::int64) kChunkFrames, totalFrames - pos);

            fillChunk (chunk, pos, frames);

            for (int ch = 0; ch < channels; ++ch)
            {
                const float* src = chunk.getReadPointer (ch);

                for (int i = 0; i < frames; ++i)
                {
                    // クランプしてから掛ける。逆にすると大きい声で折り返してクリック音になる。
                    const float v = juce::jlimit (-1.0f, 1.0f, src[i]);
                    interleaved[(size_t) i * (size_t) channels + (size_t) ch]
                        = (int16_t) std::lround (v * 32767.0f);
                }
            }

            const DWORD byteCount = (DWORD) (frames * blockAlign);

            ComPtr<IMFMediaBuffer> buffer;
            hr = MFCreateMemoryBuffer (byteCount, buffer.put());

            if (FAILED (hr))
                break;

            BYTE* dest = nullptr;
            hr = buffer->Lock (&dest, nullptr, nullptr);

            if (FAILED (hr))
                break;

            std::memcpy (dest, interleaved.data(), byteCount);
            buffer->Unlock();

            // 既定は 0。忘れると「エラーは出ないが無音の MP3」になる。
            hr = buffer->SetCurrentLength (byteCount);

            if (FAILED (hr))
                break;

            ComPtr<IMFSample> sample;
            hr = MFCreateSample (sample.put());

            if (FAILED (hr))
                break;

            hr = sample->AddBuffer (buffer.get());

            if (FAILED (hr))
                break;

            sample->SetSampleTime     (MFllMulDiv (pos,    10000000LL, sampleRate, 0));
            sample->SetSampleDuration (MFllMulDiv (frames, 10000000LL, sampleRate, 0));

            hr = sinkWriter->WriteSample (streamIndex, sample.get());

            pos += frames;
        }

        // エラー経路でも必ず Finalize する。呼ばないと途中で切れた再生不能ファイルが残る。
        const HRESULT finalizeResult = sinkWriter->Finalize();

        return FAILED (hr) ? hr : finalizeResult;
    }

    //==========================================================================
    /** 一時ファイル経由でコピーしてから本配置する。同期フォルダに書きかけを置かないため。 */
    bool copyThroughTempFile (const juce::File& source, const juce::File& destination)
    {
        auto temp = destination.getSiblingFile (destination.getFileNameWithoutExtension() + ".kvctmp");
        temp.deleteFile();

        if (! source.copyFileTo (temp))
        {
            temp.deleteFile();
            return false;
        }

        if (! moveIntoPlace (temp, destination))
        {
            temp.deleteFile();
            return false;
        }

        return true;
    }

    bool ensureParentFolder (const juce::File& destination)
    {
        auto folder = destination.getParentDirectory();

        if (! folder.isDirectory())
            folder.createDirectory();

        return folder.isDirectory();
    }
}

//==============================================================================
bool Mp3Exporter::probeMp3Available()
{
    const int cached = mp3ProbeCache.load (std::memory_order_acquire);

    if (cached >= 0)
        return cached == 1;

    bool available = false;

    if (mediaFoundationDllsPresent())
    {
        // MF のワークキューは常に MTA。JUCE のメッセージスレッドは STA なので、
        // ここだけ専用スレッドを立てて測る（インターフェースは持ち帰らない）。
        std::thread prober ([&available]
        {
            MfSession session;
            available = session.ok() && probeOnThisThread();
        });

        prober.join();
    }

    mp3ProbeCache.store (available ? 1 : 0, std::memory_order_release);
    return available;
}

//==============================================================================
Mp3Exporter::Outcome Mp3Exporter::exportBlocking (const Request& request)
{
    Outcome outcome;

    if (! request.sourceWav.existsAsFile() || request.sourceWav.getSize() <= 0)
    {
        outcome.result = Result::sourceMissing;
        outcome.japaneseMessage = juce::String (jp ("録音データが見つかりません。もう一度録音してください。"));
        return outcome;
    }

    if (! ensureParentFolder (request.destination))
    {
        outcome.result = Result::destinationFailed;
        outcome.japaneseMessage = juce::String (jp ("保存先のフォルダを作成できませんでした。\n"))
                                    + request.destination.getParentDirectory().getFullPathName()
                                    + juce::String (jp ("\n別の場所を選んでください。"));
        return outcome;
    }

    //--------------------------------------------------------------------------
    // WAV 指定。録音した一時 WAV をそのまま置くので再エンコードしない。
    if (request.format == Format::wav)
    {
        const auto destination = request.destination.withFileExtension ("wav");

        if (copyThroughTempFile (request.sourceWav, destination))
        {
            outcome.result = Result::success;
            outcome.writtenFile = destination;
            outcome.japaneseMessage = juce::String (jp ("WAV 形式で保存しました。\n"))
                                        + destination.getFullPathName();
        }
        else
        {
            outcome.result = Result::destinationFailed;
            outcome.japaneseMessage = juce::String (jp ("保存先に書き込めませんでした。\n"))
                                        + destination.getFullPathName()
                                        + juce::String (jp ("\n別の場所を選んでください。"));
        }

        return outcome;
    }

    //--------------------------------------------------------------------------
    // MP3 が無い環境（Windows N 版など）。ファイルは必ず残す。
    if (! probeMp3Available())
    {
        const auto destination = request.destination.withFileExtension ("wav");

        if (copyThroughTempFile (request.sourceWav, destination))
        {
            outcome.result = Result::mp3Unavailable;
            outcome.writtenFile = destination;
            outcome.japaneseMessage =
                juce::String (jp ("この Windows には MP3 の書き出し機能がありません。WAV 形式で保存しました。\n"))
                + destination.getFullPathName()
                + juce::String (jp ("\n\n「設定 → アプリ → オプション機能 → 機能の追加」から"
                                "「メディア機能パック」を追加すると MP3 でも保存できるようになります。"));
        }
        else
        {
            outcome.result = Result::destinationFailed;
            outcome.japaneseMessage =
                juce::String (jp ("この Windows には MP3 の書き出し機能が無く、WAV での保存にも失敗しました。\n"))
                + destination.getFullPathName()
                + juce::String (jp ("\n別の場所を選んでください。"));
        }

        return outcome;
    }

    //--------------------------------------------------------------------------
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wavFormat.createReaderFor (new juce::FileInputStream (request.sourceWav), true));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
    {
        outcome.result = Result::sourceMissing;
        outcome.japaneseMessage = juce::String (jp ("録音データを読み込めませんでした。もう一度録音してください。"));
        return outcome;
    }

    const int encodeChannels = juce::jlimit (1, 2, (int) reader->numChannels);
    const double sourceRate  = reader->sampleRate;

    int encodeRate = (int) std::lround (sourceRate);
    juce::AudioBuffer<float> resampled;   // 例外的なレートのときだけ中身が入る

    if (! isEncoderSampleRate (sourceRate))
    {
        // 88.2k / 96k / 192k の ASIO でしか通らない稀な経路。シンクライタの暗黙
        // リサンプラは当てにせず、必ず自前で 48k にしてから渡す。
        encodeRate = 48000;

        const int sourceFrames = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (1 << 28));

        juce::AudioBuffer<float> source (encodeChannels, sourceFrames + 16);
        source.clear();
        reader->read (&source, 0, sourceFrames, 0, true, encodeChannels > 1);

        const double ratio = sourceRate / (double) encodeRate;
        const int outFrames = juce::jmax (1, (int) std::floor ((double) sourceFrames / ratio));

        resampled.setSize (encodeChannels, outFrames);

        for (int ch = 0; ch < encodeChannels; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.reset();
            interpolator.process (ratio, source.getReadPointer (ch), resampled.getWritePointer (ch), outFrames);
        }
    }

    const bool usePreloaded = resampled.getNumSamples() > 0;
    const juce::int64 totalFrames = usePreloaded ? (juce::int64) resampled.getNumSamples()
                                                 : reader->lengthInSamples;

    const int bitrateKbps = clampBitrateKbps (request.bitrateKbps);
    const int avgBytesPerSecond = bitrateKbps * 1000 / 8;   // ★バイト毎秒。192 kbps = 24000

    auto tempFile = request.destination.getSiblingFile (
        request.destination.getFileNameWithoutExtension() + ".kvctmp");
    tempFile.deleteFile();

    const ChunkFiller filler = [&] (juce::AudioBuffer<float>& dest, juce::int64 startFrame, int numFrames)
    {
        if (usePreloaded)
        {
            for (int ch = 0; ch < encodeChannels; ++ch)
                dest.copyFrom (ch, 0, resampled, ch, (int) startFrame, numFrames);
        }
        else
        {
            reader->read (&dest, 0, numFrames, startFrame, true, encodeChannels > 1);
        }
    };

    HRESULT hr = E_FAIL;

    {
        MfSession session;

        if (session.ok())
            hr = encodeMp3 (tempFile, encodeChannels, encodeRate, avgBytesPerSecond, totalFrames, filler);
    }

    if (SUCCEEDED (hr) && tempFile.existsAsFile() && tempFile.getSize() > 0)
    {
        if (moveIntoPlace (tempFile, request.destination))
        {
            outcome.result = Result::success;
            outcome.writtenFile = request.destination;
            outcome.japaneseMessage = juce::String (jp ("MP3（")) + juce::String (bitrateKbps)
                                        + juce::String (jp (" kbps）で保存しました。\n"))
                                        + request.destination.getFullPathName();
            return outcome;
        }

        tempFile.deleteFile();

        outcome.result = Result::destinationFailed;
        outcome.japaneseMessage = juce::String (jp ("保存先に書き込めませんでした。\n"))
                                    + request.destination.getFullPathName()
                                    + juce::String (jp ("\n別の場所を選んでください。"));
        return outcome;
    }

    //--------------------------------------------------------------------------
    // ここまで来たらエンコード失敗。中途半端な .mp3 は絶対に残さず、WAV で救済する。
    tempFile.deleteFile();

    const auto wavDestination = request.destination.withFileExtension ("wav");

    if (copyThroughTempFile (request.sourceWav, wavDestination))
    {
        outcome.result = Result::successWithWavFallback;
        outcome.writtenFile = wavDestination;
        outcome.japaneseMessage = juce::String (jp ("MP3 で保存できなかったため、WAV 形式で保存しました。\n"))
                                    + wavDestination.getFullPathName();
    }
    else
    {
        outcome.result = Result::encodeFailed;
        outcome.japaneseMessage = juce::String (jp ("保存に失敗しました。\n"))
                                    + request.destination.getFullPathName()
                                    + juce::String (jp ("\n空き容量とフォルダの書き込み権限をご確認ください。"));
    }

    return outcome;
}

//==============================================================================
void Mp3Exporter::exportAsync (const Request& request, std::function<void (Outcome)> onFinished)
{
    // Thread::launch は完了時に自分自身を破棄する。MF 用の MTA 初期化は
    // exportBlocking の中で行うので、ここでは何もしない。
    juce::Thread::launch ([request, onFinished]
    {
        auto outcome = exportBlocking (request);

        juce::MessageManager::callAsync ([onFinished, outcome]
        {
            if (onFinished != nullptr)
                onFinished (outcome);
        });
    });
}

//==============================================================================
juce::String Mp3Exporter::makeDefaultFileName (Format format)
{
    juce::String name (jp ("ボイチェン_"));
    name << juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M");
    name << (format == Format::wav ? ".wav" : ".mp3");
    return name;
}

juce::File Mp3Exporter::getDefaultExportFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    if (! base.isDirectory())
        base = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    if (! base.isDirectory())
        base = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    auto folder = base.getChildFile (jp ("簡単ボイチェン"));

    if (! folder.isDirectory())
        folder.createDirectory();

    return folder.isDirectory() ? folder : base;
}

} // namespace kvc
