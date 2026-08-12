// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/DeviceCatalog.h"
#include "core/BeepGenerator.h"
#include "core/DriftRing.h"
#include "core/Parameters.h"
#include "core/PeakRing.h"
#include "core/RawRing.h"
#include "core/VoiceEngine.h"

#include <atomic>
#include <memory>

namespace kvc
{

class Recorder;

//==============================================================================
/** デバイスグラフの生成・起動・停止と、全オーディオコールバックの実装。

    信号フロー（順序が意味を持つ。並べ替え禁止）:

      マスター = 入力デバイスのコールバック。DSP はここでしか動かない。
        1. 入力チャンネルをモノラル化
        2. PeakRing へ（波形・レベルメーター）
        3. VoiceEngine::process  … HPF → ノイズ除去 → PSOLA → 出力トリム
        4. 録音タップ Recorder::writeBlock          ← DSP 後・ミュート前・ビープ前
        5. 送信バス: sendRing.write(muted ? 無音 : processed)   ← ★ビープより前
        6. モニターバス: processed(+ミュート設定次第で無音) に BeepGenerator を加算
           → SoftLimiter → collapse なら outputChannelData へ直接、
             別デバイスなら monitorRing.write()

      スレーブ = 出力デバイスのコールバック。ring.read() だけ（ここでドリフト補正が走る）。

    ★ビープが相手に届かない保証は構造である
      送信側のコードパスに BeepGenerator への参照が 1 つも無い。実行時の if で
      守っているのではない。レビュー時はこの不変条件を最優先で確認すること。

    collapse ルール:
      入力とモニターが同一エンドポイントなら 1 つの AudioIODevice を
      入出力両方で開く。同一クロックなのでドリフト補正もリングも不要になり、
      モニター遅延が純粋なデバイス遅延だけになる。これが「遅延ゼロに感じる」に
      唯一到達しうる構成なので、UI もここへ誘導する。
      逆に、異なる物理エンドポイント間で JUCE の「結合 WASAPI デバイス」を
      作ってはいけない（JUCE 側にドリフト補正が無く、溢れたらブロック破棄、
      枯れたら無音になる）。

    起動・停止順序:
      起動 = 全デバイス open → 各リングに targetFill ぶんの無音プリフィル
             → スレーブ start → 最後にマスター start
      停止 = マスター stop → スレーブ stop → リング破棄
      再構成は必ずグラフ全体を作り直す。1 レグだけの差し替えはしない。

    スレッド契約:
      start / stop / restartAsync / setRecorderTap / 再生系 ... メッセージスレッド専用
      getStatus / 各 Diagnostics                             ... どのスレッドからでも
      内部のコールバック群                                    ... 各デバイスのオーディオスレッド
*/
class AudioEngine : private DeviceCatalog::Listener,
                    private juce::AsyncUpdater
{
public:
    struct GraphRequest
    {
        juce::String inputEndpointId;
        juce::String monitorEndpointId;
        juce::String sendEndpointId;    ///< 空なら「送信しない（モニターのみ）」

        bool allowAsio = false;         ///< 詳細設定でオプトインしたときだけ true
        bool allowExclusive = false;

        int  requestedBlockSize = 128;  ///< 実際に取れた値は必ず読み戻すこと
        double requestedSampleRate = kPreferredSampleRate;

        DriftRing::FillMode fillMode = DriftRing::FillMode::stable;
        NoiseQuality noiseQuality = NoiseQuality::lowLatency256;

        /** 内蔵 PSOLA の代わりに使う外部プラグイン。nullptr なら内蔵。
            所有者は呼び出し側（MainComponent）。start() 中は生きていること。
            差し替えは必ず stop() してから。実行中に遅延が変わってはならないため。 */
        ExternalProcessor* external = nullptr;

        /** ノイズ除去を信号経路に置くか。OFF なら STFT ぶんの遅延（5.3 ms）が消える。
            UI のトグルを切り替えたらグラフの作り直しが要る。 */
        bool noiseSuppressionInPath = true;

        /** 使う入力チャンネル。
            -1 = 先頭 2 本をミックス（ステレオマイク向け）
             0 以上 = そのチャンネル 1 本だけを使う

            ★オーディオインターフェースでは必須。MOTU M6 は ASIO で入力を 10 本
              （In 1-6 / Loopback / Loopback Mix）出すので、先頭 2 本固定では
              In 3 以降のマイクが一切拾えない。さらに In 1 だけにマイクがある構成で
              先頭 2 本をミックスすると、無音の In 2 と平均されてレベルが半分になる。 */
        int inputChannelIndex = 0;
    };

    struct LegStatus
    {
        bool     active = false;
        juce::String deviceName;
        Backend  backend = Backend::unknown;
        double   sampleRate = 0.0;
        int      blockSize = 0;
        int      deviceLatencySamples = 0;
        bool     latencyReportedByDriver = false;   ///< false なら「約」を付けて表示する

        /** そのデバイスが持つ入力チャンネル名の全リスト（開いた本数ではない）。
            UI のチャンネル選択はこれを出す。 */
        juce::StringArray availableInputChannels;
    };

    struct Status
    {
        bool running = false;
        bool collapsed = false;         ///< 入力とモニターが 1 デバイスに畳まれている

        LegStatus input, monitor, send;

        int    dspLatencySamples = 0;   ///< 768（既定）。実行中は不変。
        int    monitorTotalLatencySamples = 0;
        double monitorTotalLatencyMs = 0.0;
        bool   latencyIsEstimate = false;   ///< 仮想デバイスが嘘の値を返した等

        juce::String lastErrorJapanese;
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        /** メッセージスレッド。起動・停止・デバイス変化のたび。 */
        virtual void engineStatusChanged() {}
        /** メッセージスレッド。日本語のユーザー向け文言がそのまま入る。 */
        virtual void engineErrorOccurred (const juce::String& japaneseMessage) { juce::ignoreUnused (japaneseMessage); }
        /** メッセージスレッド。使用中のデバイスが消えた等でグラフが落ちた。 */
        virtual void engineDeviceListChanged() {}
    };

    //==========================================================================
    AudioEngine (DeviceCatalog&, VoiceParams&);
    ~AudioEngine() override;

    /** グラフを作って動かす。既に動いていたら一旦全部止めて作り直す。
        戻り値 false のときは getStatus().lastErrorJapanese に理由が入る。 */
    bool start (const GraphRequest&);

    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

    /** 最後に成功した GraphRequest で作り直す。ノイズ除去の品質を変えたときなど、
        アルゴリズム遅延が変わる設定変更のあとは必ずこれを呼ぶ。 */
    void restartAsync();

    Status getStatus() const;

    /** 録音タップ。エンジン停止中に一度だけ設定すること（寿命の競合を避けるため）。 */
    void setRecorderTap (Recorder*) noexcept;

    PeakRing&       getInputPeakRing() noexcept       { return inputPeaks; }
    const PeakRing& getInputPeakRing() const noexcept { return inputPeaks; }

    /** スペクトル表示用の生サンプル。PeakRing は間引き済みで FFT に使えないため別に持つ。 */
    RawRing&        getInputRawRing() noexcept        { return inputRaw; }
    const RawRing&  getInputRawRing() const noexcept  { return inputRaw; }

    /** 変換後の声（ミュート前・ビープ前）。声質判定はこれを見る。 */
    const RawRing&  getOutputRawRing() const noexcept { return outputRaw; }

    /** ホットキーハンドラが trigger() を呼ぶ。加算はモニターバスでのみ行われる。 */
    BeepGenerator& getBeepGenerator() noexcept { return beep; }

    DriftRing::Diagnostics getSendDiagnostics() const noexcept;
    DriftRing::Diagnostics getMonitorDiagnostics() const noexcept;
    bool hasSendRing() const noexcept;
    bool hasMonitorRing() const noexcept;

    //==========================================================================
    /** ハウリング／暴走からの聴覚保護。

        SoftLimiter は -1 dBFS で頭を押さえるだけなので、ハウリングが起きると
        「-1 dBFS の連続音」がヘッドホンに出続ける。これは十分に危険な音量で、
        実際にウェブカメラのマイクへ切り替えた際に発生した。
        リミッタとは別に、フルスケールに張り付き続けたら音を切る仕組みが要る。

        マイクとスピーカーの音響的な回り込みは、デバイスの組み合わせを検査しても
        検出できない（同じ機器を使っていなくても物理的に回り込む）。
        出力そのものを監視するしかない。 */
    bool isMonitorProtectionEngaged() const noexcept
    { return monitorProtection.load (std::memory_order_relaxed); }

    /** 保護を解除して音を戻す。メッセージスレッド専用。 */
    void clearMonitorProtection() noexcept;

    //==========================================================================
    /** 申告遅延が最小になるバッファサイズを実測で探す。

        ★ドライバの申告遅延はバッファサイズに対して単調ではない。
          MOTU M Series は 64 のとき in+out=294 サンプルなのに、128 では 454 に増える
          （出力側が 203 → 299 に跳ねる）。つまり「バッファを大きくすれば安定して
          遅延が増えるだけ」という前提が成り立たない。計算では分からないので、
          実際に開いて聞くしかない。

        エンジン停止中にメッセージスレッドから呼ぶこと。
        デバイスを何度も開き直すので数秒かかる。
        見つからなければ 0（=既定に任せる）を返す。 */
    struct BufferProbeResult
    {
        int bestBlockSize = 0;
        int bestTotalLatencySamples = 0;
        juce::String summaryJapanese;
    };

    static BufferProbeResult probeBestBufferSize (DeviceCatalog&,
                                                  const juce::String& endpointId,
                                                  bool allowAsio, bool allowExclusive,
                                                  double preferredRate);

    /** Windows 11 のマイクプライバシー拒否は「開けるが完全な無音が来る」形で現れる。
        3 秒以上ぴったり無音なら true。UI は ms-settings:privacy-microphone を案内する。 */
    bool isSilentInputSuspected() const noexcept;

    //==========================================================================
    // 録音テイクの試聴。モニターバスにだけ混ざる（送信には行かない）。

    bool   startTakePlayback (const juce::File& wavFile);
    void   stopTakePlayback();
    bool   isPlayingTake() const noexcept;
    double getPlaybackPositionSeconds() const noexcept;
    double getPlaybackLengthSeconds() const noexcept;

    void addListener (Listener*);
    void removeListener (Listener*);

    //==========================================================================
    /** sharedLowLatency → shared のはしご。sharedLowLatency は IAudioClient3 非対応
        ドライバでフォールバックせずに作成失敗するので、自前で降りる必要がある。 */
    static juce::Array<Backend> makeFallbackLadder (Backend preferred, bool allowAsio, bool allowExclusive);

private:
    //==========================================================================
    /** 入力デバイス（collapse 時は入出力兼用）のコールバック。 */
    class MasterCallback final : public juce::AudioIODeviceCallback
    {
    public:
        explicit MasterCallback (AudioEngine& o) : owner (o) {}
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override;
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceError (const juce::String&) override;
    private:
        AudioEngine& owner;
    };

    /** モニターまたは送信のスレーブ。ring.read() 以外は何もしない。
        ★送信用インスタンスからは BeepGenerator に一切触れない。 */
    class SlaveCallback final : public juce::AudioIODeviceCallback
    {
    public:
        SlaveCallback (AudioEngine& o, DriftRing& r, bool isMonitorLeg)
            : owner (o), ring (r), monitorLeg (isMonitorLeg) {}
        void audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override;
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceError (const juce::String&) override;
    private:
        AudioEngine& owner;
        DriftRing&   ring;
        const bool   monitorLeg;
    };

    void deviceListChanged() override;   // DeviceCatalog::Listener
    void handleAsyncUpdate() override;   // restartAsync の実体

    void reportError (const juce::String& japaneseMessage);
    void teardown();

    //==========================================================================
    DeviceCatalog& catalog;
    VoiceParams&   params;

    std::unique_ptr<juce::AudioIODevice> masterDevice;   // 入力（collapse 時は入出力）
    std::unique_ptr<juce::AudioIODevice> monitorDevice;  // collapse 時は nullptr
    std::unique_ptr<juce::AudioIODevice> sendDevice;     // 「送信しない」なら nullptr

    MasterCallback masterCallback { *this };
    std::unique_ptr<SlaveCallback> monitorCallback, sendCallback;

    DriftRing monitorRing, sendRing;

    VoiceEngine   voice;
    BeepGenerator beep;
    SoftLimiter   monitorLimiter;
    PeakRing      inputPeaks;
    RawRing       inputRaw;
    RawRing       outputRaw;

    // オーディオスレッドで使う作業バッファ。prepare で一度だけ確保する。
    juce::AudioBuffer<float> monoIn, processed, monitorBus;

    std::atomic<Recorder*> recorderTap { nullptr };
    std::atomic<bool>      running { false };
    std::atomic<bool>      collapsed { false };
    std::atomic<int>       silentInputBlocks { 0 };

    /** 使う入力チャンネル。-1 = 届いた全チャンネルをミックス。
        デバイスは 0 からこの番号までを連続で開いているので、ここは必ず有効域に入る。 */
    std::atomic<int>       inputChannelPick { -1 };

    //==========================================================================
    // 聴覚保護。フラグだけが共有で、残りはオーディオスレッド専有。
    std::atomic<bool> monitorProtection { false };

    /** 0 → 1 のソフトスタートと、保護作動時の 20 ms フェードアウトに使う。
        起動直後にいきなり全開で出さないための物でもある。 */
    float monitorSafetyGain = 0.0f;
    int   loudRunSamples = 0;

    /** オーディオスレッド専用。モニターバス最終段（リミッタの後）で呼ぶ。 */
    void applyMonitorSafety (float* const* channels, int numChannels, int numSamples) noexcept;

    /** マスターの実測サンプルレート。status は statusLock 越しなので
        オーディオスレッドから読めない。無音検出と試聴の秒換算に要る。 */
    std::atomic<double> masterSampleRate { kPreferredSampleRate };

    // テイク試聴。juce::AudioTransportSource は使わない:
    // getNextAudioBlock() が callbackLock（CriticalSection）を取り、
    // UI が位置表示のために呼ぶ getCurrentPosition() も同じロックを取るため、
    // 「オーディオコールバックで mutex 禁止」が実際に破れる（優先度逆転）。
    // 代わりにテイク全体をメッセージスレッドで展開し、atomic カーソルで読む。
    juce::AudioFormatManager formatManager;
    juce::AudioBuffer<float> playbackBuffer;          // メッセージスレッドでのみ確保する
    std::atomic<int>  playbackLengthSamples { 0 };
    std::atomic<int>  playbackPos { 0 };
    std::atomic<bool> playbackActive { false };
    /** バッファを差し替える前に、進行中のコールバックが抜けるのを待つための旗。 */
    std::atomic<bool> playbackInCallback { false };

    GraphRequest lastRequest;
    Status status;
    juce::CriticalSection statusLock;    // メッセージスレッド同士のみ。オーディオスレッドからは触らない。

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace kvc
