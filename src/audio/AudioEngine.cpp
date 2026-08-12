// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "audio/AudioEngine.h"

#include "audio/Recorder.h"

#include <cmath>


namespace
{
    // JUCE の String(const char*) は ASCII 専用（CharPointer_ASCII）。
    // UTF-8 の日本語リテラルは必ずこれを通す。通さないと 1 バイトずつ別文字に化ける。
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

namespace kvc
{

namespace
{

/** 各レグで実際に開くチャンネル数の上限。内部はモノラルなので、入力は
    モノラル化の材料として、出力はファンアウト先として 2 本あれば足りる。 */
constexpr int kMaxLegChannels = 2;

/** テイク試聴のためにメモリへ展開する最大長。48 kHz モノラルで約 115 MB。
    確かめ録りは数十秒の想定なので、これを超えるテイクは頭から切って鳴らす。 */
constexpr double kMaxTakePlaybackSeconds = 600.0;

/** 無音検出のカウンタが桁あふれしないための頭打ち（3 秒判定にはこれで十分）。 */
constexpr int kSilentSampleCap = 1 << 24;

/** ハウリング判定。リミッタの閾値（0.891）より少し下に置き、
    そこへ 8 割以上のサンプルが張り付く状態が 0.5 秒続いたら作動させる。
    通常の発声は瞬間的にしかここへ届かないので誤作動しない。 */
constexpr float kHowlLevel = 0.85f;
constexpr int   kHowlSamplesToEngage = 12000;      // 48 kHz で 0.25 秒

/** 1 サンプルあたりのゲイン変化。48 kHz で out=20 ms / in=200 ms 相当。 */
constexpr float kSafetyFadeOutStep = 1.0f / (0.020f * 48000.0f);
constexpr float kSafetyFadeInStep  = 1.0f / (0.200f * 48000.0f);

//==============================================================================
juce::BigInteger makeChannelMask (int numAvailable, int numWanted)
{
    juce::BigInteger mask;

    for (int i = 0, n = juce::jmin (numAvailable, numWanted); i < n; ++i)
        mask.setBit (i);

    return mask;
}

double pickSampleRate (juce::AudioIODevice& device, double preferred)
{
    const auto rates = device.getAvailableSampleRates();

    if (rates.isEmpty() || rates.contains (preferred))
        return preferred;

    double best = rates.getFirst();
    double bestDistance = std::abs (best - preferred);

    for (const auto rate : rates)
    {
        const double distance = std::abs (rate - preferred);

        if (distance < bestDistance)
        {
            best = rate;
            bestDistance = distance;
        }
    }

    return best;
}

/** desired <= 0 は「このデバイスの既定に任せる」。送信レグがこれ。仮想ケーブル相手に
    低遅延を追っても体感は変わらず、大きめのバッファは純粋に安定側に効く。 */
int pickBlockSize (juce::AudioIODevice& device, int desired)
{
    const int deviceDefault = device.getDefaultBufferSize();

    if (desired <= 0)
        desired = deviceDefault > 0 ? deviceDefault : 480;

    const auto sizes = device.getAvailableBufferSizes();

    if (sizes.isEmpty())
        return deviceDefault > 0 ? deviceDefault : desired;

    int best = -1;

    for (const auto size : sizes)
        if (size >= desired && (best < 0 || size < best))
            best = size;

    if (best < 0)
        for (const auto size : sizes)
            best = juce::jmax (best, size);

    return best;
}

//==============================================================================
struct LegOpenResult
{
    std::unique_ptr<juce::AudioIODevice> device;
    Backend backend = Backend::unknown;
    juce::String driverError;   ///< ドライバが返した文字列。日本語文に括弧書きで添える。
};

/** はしごを上から順に試す。inEntry / outEntry のどちらかが nullptr なら片方向で開く。
    両方を渡すのは collapse 構成のときだけ。 */
LegOpenResult openLeg (DeviceCatalog& catalog,
                       const DeviceEntry* inEntry,
                       const DeviceEntry* outEntry,
                       bool allowAsio, bool allowExclusive,
                       double preferredRate,
                       int desiredBlock,
                       int inputChannelIndex = -1)
{
    LegOpenResult result;

    const Backend preferred = inEntry  != nullptr ? inEntry->bestBackend
                            : outEntry != nullptr ? outEntry->bestBackend
                                                  : Backend::unknown;

    for (const auto backend : AudioEngine::makeFallbackLadder (preferred, allowAsio, allowExclusive))
    {
        auto* type = catalog.getType (backend);

        if (type == nullptr)
            continue;

        const juce::String inName  = inEntry  != nullptr ? inEntry->getJuceNameFor  (backend) : juce::String();
        const juce::String outName = outEntry != nullptr ? outEntry->getJuceNameFor (backend) : juce::String();

        // このバックエンドがそのエンドポイントを列挙していない。
        if ((inEntry != nullptr && inName.isEmpty()) || (outEntry != nullptr && outName.isEmpty()))
            continue;

        // ASIO は入出力を分離できない（hasSeparateInputsAndOutputs() == false）。
        // 別々のハードを 1 デバイスとして開こうとすると壊れるので、ここで捨てる。
        if (! type->hasSeparateInputsAndOutputs()
            && inName.isNotEmpty() && outName.isNotEmpty() && inName != outName)
            continue;

        std::unique_ptr<juce::AudioIODevice> device (type->createDevice (outName, inName));

        if (device == nullptr)
            continue;

        const int availableIn = device->getInputChannelNames().size();

        juce::BigInteger inMask;

        if (inEntry != nullptr)
        {
            // ★疎なマスク（目的の 1 ビットだけを立てる）は使ってはいけない。
            //   バックエンドによっては、開いていないチャンネルぶんの未初期化バッファが
            //   そのままコールバックへ渡され、フルスケールの雑音になる。
            //   C922 で「入力ch を 1 本指定すると爆音」という形で実際に発生した。
            //   0 から目的のチャンネルまでを必ず連続で開き、どれを使うかは
            //   コールバック側（inputChannelPick）で選ぶ。
            const int wanted = inputChannelIndex >= 0
                                   ? juce::jmax (inputChannelIndex + 1, kMaxLegChannels)
                                   : kMaxLegChannels;

            inMask = makeChannelMask (availableIn, wanted);
        }

        const auto outMask = makeChannelMask (device->getOutputChannelNames().size(),
                                              outEntry != nullptr ? kMaxLegChannels : 0);

        if ((inEntry != nullptr && inMask.isZero()) || (outEntry != nullptr && outMask.isZero()))
        {
            result.driverError = jp ("使用できるチャンネルがありません");
            continue;
        }

        const double rate  = pickSampleRate (*device, preferredRate);
        const int    block = pickBlockSize  (*device, desiredBlock);

        const juce::String error = device->open (inMask, outMask, rate, block);

        if (error.isEmpty() && device->isOpen())
        {
            result.device = std::move (device);
            result.backend = backend;
            result.driverError.clear();
            return result;
        }

        result.driverError = error.isNotEmpty() ? error : device->getLastError();
        device->close();
    }

    return result;
}

AudioEngine::LegStatus makeLegStatus (juce::AudioIODevice& device,
                                      const juce::String& friendlyName,
                                      Backend backend,
                                      bool inputSide)
{
    AudioEngine::LegStatus leg;
    leg.active = true;
    leg.deviceName = friendlyName;
    leg.backend = backend;
    leg.sampleRate = device.getCurrentSampleRate();
    leg.blockSize = device.getCurrentBufferSizeSamples();

    // JUCE の WASAPI 実装は latencyIn / latencyOut に currentBufferSizeSamples を
    // 既に足し込んでいる。ここで再度ブロック長を加えると二重計上になる。
    const int reported = inputSide ? device.getInputLatencyInSamples()
                                   : device.getOutputLatencyInSamples();

    if (inputSide)
        leg.availableInputChannels = device.getInputChannelNames();

    leg.latencyReportedByDriver = reported > 0;
    // 仮想デバイスは 0 を返すことがある。その場合だけブロック 2 個ぶんで代用し、
    // UI 側が「約」を付けられるように latencyReportedByDriver を false のままにする。
    leg.deviceLatencySamples = reported > 0 ? reported : juce::jmax (0, leg.blockSize) * 2;

    return leg;
}

juce::String withDriverDetail (const juce::String& japanese, const juce::String& driverError)
{
    return driverError.isEmpty() ? japanese
                                 : japanese + jp ("\n（ドライバの応答: ") + driverError + jp ("）");
}

} // namespace

//==============================================================================
AudioEngine::AudioEngine (DeviceCatalog& catalogToUse, VoiceParams& paramsToUse)
    : catalog (catalogToUse), params (paramsToUse)
{
    formatManager.registerBasicFormats();
    catalog.addListener (this);
}

AudioEngine::~AudioEngine()
{
    cancelPendingUpdate();
    catalog.removeListener (this);
    teardown();
}

//==============================================================================
juce::Array<Backend> AudioEngine::makeFallbackLadder (Backend preferred, bool allowAsio, bool allowExclusive)
{
    juce::Array<Backend> ladder;

    const auto add = [&] (Backend backend)
    {
        if (backend == Backend::unknown)
            return;
        if (backend == Backend::asio && ! allowAsio)
            return;
        if (backend == Backend::wasapiExclusive && ! allowExclusive)
            return;
        if (! ladder.contains (backend))
            ladder.add (backend);
    };

    add (preferred);

    // ASIO / 排他が開けなかったら必ず共有系へ降りる（MOTU ASIO を SYNCROOM と
    // 取り合って「ボイチェンを入れたら SYNCROOM が壊れた」になるのを防ぐ）。
    // 逆向きに、共有系から ASIO / 排他へ勝手に上がることは決してしない。
    add (Backend::wasapiLowLatency);
    add (Backend::wasapiShared);

    return ladder;
}

//==============================================================================
bool AudioEngine::start (const GraphRequest& request)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    teardown();

    const int dspLatency = dspLatencySamples (request.noiseQuality);

    const auto fail = [this, dspLatency] (const juce::String& message) -> bool
    {
        teardown();

        {
            const juce::ScopedLock sl (statusLock);
            status = Status{};
            status.dspLatencySamples = dspLatency;
        }

        reportError (message);
        listeners.call ([] (Listener& l) { l.engineStatusChanged(); });
        return false;
    };

    //--------------------------------------------------------------------------
    // 構成の妥当性。ハウリング系の構成はここで必ず止める（聴覚障害リスクがある）。
    const auto validation = catalog.validateRouting (request.inputEndpointId,
                                                     request.monitorEndpointId,
                                                     request.sendEndpointId);

    if (validation.blocksStart)
        return fail (validation.japaneseMessage);

    const auto* inputEntry = catalog.findInput (request.inputEndpointId);

    if (inputEntry == nullptr)
        return fail (jp ("マイク（入力）のデバイスが見つかりません。"
                     "「機器を再検索」を押すか、機器を選び直してください。"));

    const auto* monitorEntry = catalog.findOutput (request.monitorEndpointId);

    if (monitorEntry == nullptr)
        return fail (jp ("「自分に聞こえる音」のデバイスが見つかりません。"
                     "「機器を再検索」を押すか、機器を選び直してください。"));

    const DeviceEntry* sendEntry = nullptr;

    if (request.sendEndpointId.isNotEmpty())
    {
        sendEntry = catalog.findOutput (request.sendEndpointId);

        if (sendEntry == nullptr)
            return fail (jp ("「相手に送る音」のデバイスが見つかりません。"
                         "「機器を再検索」を押すか、「送信しない（モニターのみ）」を選んでください。"));
    }

    //--------------------------------------------------------------------------
    // マスター = 入力デバイス。collapse できるなら入出力を 1 デバイスで開く。
    bool wantCollapse = DeviceCatalog::isCollapsible (request.inputEndpointId,
                                                      request.monitorEndpointId);

    LegOpenResult master;

    if (wantCollapse)
    {
        master = openLeg (catalog, inputEntry, monitorEntry,
                          request.allowAsio, request.allowExclusive,
                          request.requestedSampleRate, request.requestedBlockSize,
                          request.inputChannelIndex);

        // 1 デバイスで開けなかっただけなので、2 本に分けてやり直す。
        if (master.device == nullptr)
            wantCollapse = false;
    }

    if (master.device == nullptr)
        master = openLeg (catalog, inputEntry, nullptr,
                          request.allowAsio, request.allowExclusive,
                          request.requestedSampleRate, request.requestedBlockSize,
                          request.inputChannelIndex);

    if (master.device == nullptr)
        return fail (withDriverDetail (jp ("マイク（入力）「") + inputEntry->friendlyName + jp ("」を開けませんでした。\n"
                                       "ほかのアプリが排他モードで使用しているか、"
                                       "Windows のプライバシー設定でマイクへのアクセスが許可されていない可能性があります。"),
                                       master.driverError));

    const double masterRate  = master.device->getCurrentSampleRate();
    const int    masterBlock = master.device->getCurrentBufferSizeSamples();

    if (masterRate <= 0.0 || masterBlock <= 0 || masterBlock > kMaxBlockSize)
        return fail (jp ("マイク（入力）「") + inputEntry->friendlyName + jp ("」が返したバッファ長（")
                     + juce::String (masterBlock) + jp (" サンプル）には対応できません。"
                     "詳細設定でバッファサイズを変更してください。"));

    //--------------------------------------------------------------------------
    // モニタースレーブ。collapse していないときだけ存在する。
    LegOpenResult monitor;

    if (! wantCollapse)
    {
        // ★スレーブに masterBlock*2 を要求してはいけない。
        //   入力が WASAPI 共有（480）だと 960 を要求することになり、ASIO 側が
        //   1024 へ切り上げて単独で 21 ms 増える（実測 84 ms の主因だった）。
        //   安定性は DriftRing の targetFill（master + slave + 32）が担保しているので、
        //   スレーブは利用者が指定した小さいバッファをそのまま要求してよい。
        const int monitorDesired = request.requestedBlockSize > 0 ? request.requestedBlockSize
                                                                  : masterBlock;

        monitor = openLeg (catalog, nullptr, monitorEntry,
                           request.allowAsio, request.allowExclusive,
                           masterRate, monitorDesired);

        if (monitor.device == nullptr)
            return fail (withDriverDetail (jp ("「自分に聞こえる音」のデバイス「") + monitorEntry->friendlyName
                                           + jp ("」を開けませんでした。\n"
                                           "ほかのアプリが排他モードで使用している可能性があります。"),
                                           monitor.driverError));

        if (monitor.device->getCurrentBufferSizeSamples() > kMaxBlockSize)
            return fail (jp ("「自分に聞こえる音」のデバイス「") + monitorEntry->friendlyName
                         + jp ("」が返したバッファ長には対応できません。"));
    }

    //--------------------------------------------------------------------------
    // 送信スレーブ。バッファはデバイス既定に任せる（安定優先）。
    LegOpenResult send;

    if (sendEntry != nullptr)
    {
        // 送信側の遅延は相手に聞こえる遅れそのもの（SYNCROOM の合奏で効く）。
        // 仮想ケーブルは小さいバッファでも安定するので、ここも詰める。
        // ただし実機ほど攻めず 128 を下限にする。
        const int sendDesired = juce::jmax (128, request.requestedBlockSize > 0
                                                     ? request.requestedBlockSize : 128);

        send = openLeg (catalog, nullptr, sendEntry,
                        request.allowAsio, request.allowExclusive,
                        masterRate, sendDesired);

        if (send.device == nullptr)
            return fail (withDriverDetail (jp ("「相手に送る音」のデバイス「") + sendEntry->friendlyName
                                           + jp ("」を開けませんでした。\n"
                                           "SYNCROOM や OBS など、ほかのアプリが同じ仮想ケーブルを"
                                           "使っていないか確認してください。\n"
                                           "「送信しない（モニターのみ）」を選べば、自分の声の確認だけはできます。"),
                                           send.driverError));

        if (send.device->getCurrentBufferSizeSamples() > kMaxBlockSize)
            return fail (jp ("「相手に送る音」のデバイス「") + sendEntry->friendlyName
                         + jp ("」が返したバッファ長には対応できません。"));
    }

    //--------------------------------------------------------------------------
    // 確保はすべてここで済ませる。以降オーディオスレッドは一切確保しない。
    // ドライバが公称より大きいブロックを渡してきても落ちないように 2 倍の余裕を持たせる。
    const int maxBlock = juce::jlimit (64, kMaxBlockSize, juce::jmax (masterBlock * 2, 512));

    voice.prepare ({ masterRate, maxBlock, request.noiseQuality,
                     request.external, request.noiseSuppressionInPath });
    beep.prepare (masterRate);
    beep.resetPlayback();
    monitorLimiter.prepare (masterRate);
    monitorLimiter.reset();

    inputPeaks.reset();
    silentInputBlocks.store (0, std::memory_order_relaxed);
    masterSampleRate.store (masterRate, std::memory_order_relaxed);

    monoIn.setSize     (kInternalNumChannels, maxBlock, false, true, false);
    processed.setSize  (kInternalNumChannels, maxBlock, false, true, false);
    monitorBus.setSize (kInternalNumChannels, maxBlock, false, true, false);
    monoIn.clear();
    processed.clear();
    monitorBus.clear();

    // 実測レートは要求値と違いうる。録音の WAV ヘッダが狂うと後から直せない。
    if (auto* recorder = recorderTap.load (std::memory_order_acquire))
        if (! recorder->isRecording())
            recorder->prepare (masterRate, kInternalNumChannels);

    //--------------------------------------------------------------------------
    // リングは必ず「実際に開けた」レートとブロック長で組む。要求値で組むと
    // 128 を頼んで 480 が返ってきた瞬間に恒常的アンダーランになる。
    if (send.device != nullptr)
    {
        sendRing.prepare (kInternalNumChannels,
                          masterRate, send.device->getCurrentSampleRate(),
                          masterBlock, send.device->getCurrentBufferSizeSamples(),
                          request.fillMode);
        sendRing.prefillSilence();
        sendCallback = std::make_unique<SlaveCallback> (*this, sendRing, false);
    }

    if (monitor.device != nullptr)
    {
        monitorRing.prepare (kInternalNumChannels,
                             masterRate, monitor.device->getCurrentSampleRate(),
                             masterBlock, monitor.device->getCurrentBufferSizeSamples(),
                             request.fillMode);
        monitorRing.prefillSilence();
        monitorCallback = std::make_unique<SlaveCallback> (*this, monitorRing, true);
    }

    masterDevice  = std::move (master.device);
    monitorDevice = std::move (monitor.device);
    sendDevice    = std::move (send.device);

    collapsed.store (wantCollapse, std::memory_order_relaxed);

    // 0 からこの番号までを連続で開いているので、そのまま添字として使える。
    inputChannelPick.store (request.inputChannelIndex, std::memory_order_relaxed);

    // 起動のたびに 0 から立ち上げる。前回ハウリングで切れていた状態も持ち越さない。
    monitorProtection.store (false, std::memory_order_relaxed);
    monitorSafetyGain = 0.0f;
    loudRunSamples = 0;

    // 聴覚保護をリセットし、0 から立ち上げる（ソフトスタート）。
    // 起動・機器変更の瞬間にいきなり全開で出さないため。
    monitorProtection.store (false, std::memory_order_relaxed);
    monitorSafetyGain = 0.0f;
    loudRunSamples = 0;

    // running はマスター start より前に release で立てる。これで、running を acquire で
    // 見たスレッドからはデバイスポインタ群も確実に見える。
    running.store (true, std::memory_order_release);

    // 起動順: 無音プリフィル済のスレーブが先、マスターは最後。逆にするとマスターが
    // 走り出した瞬間に空のリングを読みに行ってアンダーランする。
    if (sendDevice != nullptr)    sendDevice->start (sendCallback.get());
    if (monitorDevice != nullptr) monitorDevice->start (monitorCallback.get());
    masterDevice->start (&masterCallback);

    //--------------------------------------------------------------------------
    Status fresh;
    fresh.running = true;
    fresh.collapsed = wantCollapse;
    // ★prepare 済みの VoiceEngine から実際の値を取る。
    //   dspLatencySamples() は「内蔵 PSOLA 前提」の固定値（512+256）で、
    //   外部プラグイン使用時の実態と合わない。実際 Pitchproof の遅延は 0 サンプルなのに
    //   16 ms 遅れていることになっており、原因追及を丸ごと誤らせていた。
    fresh.dspLatencySamples = voice.getLatencySamples();

    fresh.input = makeLegStatus (*masterDevice, inputEntry->friendlyName, master.backend, true);

    fresh.monitor = wantCollapse
                        ? makeLegStatus (*masterDevice, monitorEntry->friendlyName, master.backend, false)
                        : makeLegStatus (*monitorDevice, monitorEntry->friendlyName, monitor.backend, false);

    if (sendDevice != nullptr)
        fresh.send = makeLegStatus (*sendDevice, sendEntry->friendlyName, send.backend, false);

    fresh.monitorTotalLatencySamples = fresh.input.deviceLatencySamples
                                     + fresh.dspLatencySamples
                                     + fresh.monitor.deviceLatencySamples
                                     + (wantCollapse ? 0 : monitorRing.getTargetFill());

    fresh.monitorTotalLatencyMs = 1000.0 * static_cast<double> (fresh.monitorTotalLatencySamples) / masterRate;
    fresh.latencyIsEstimate = ! (fresh.input.latencyReportedByDriver && fresh.monitor.latencyReportedByDriver);

    lastRequest = request;

    {
        const juce::ScopedLock sl (statusLock);
        status = fresh;
    }

    listeners.call ([] (Listener& l) { l.engineStatusChanged(); });
    return true;
}

//==============================================================================
void AudioEngine::applyMonitorSafety (float* const* channels, int numChannels,
                                      int numSamples) noexcept
{
    if (channels == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    // 数値が壊れた場合の保険。NaN/Inf をそのまま出すと DA で最大音量の雑音になる。
    for (int c = 0; c < numChannels; ++c)
        for (int i = 0; i < numSamples; ++i)
            if (! std::isfinite (channels[c][i]))
                channels[c][i] = 0.0f;

    if (! monitorProtection.load (std::memory_order_relaxed))
    {
        // 「フルスケールに張り付いている」を数える。通常の声は瞬間的にしか
        // ここへ届かないので、連続して張り付くのはハウリングか暴走だけ。
        int loud = 0;

        for (int i = 0; i < numSamples; ++i)
            if (std::fabs (channels[0][i]) > kHowlLevel)
                ++loud;

        if (loud * 10 >= numSamples * 8)     // ブロックの 8 割以上
            loudRunSamples += numSamples;
        else
            loudRunSamples = juce::jmax (0, loudRunSamples - numSamples * 2);

        if (loudRunSamples >= kHowlSamplesToEngage)
        {
            monitorProtection.store (true, std::memory_order_relaxed);
            loudRunSamples = 0;
        }
    }

    const float target = monitorProtection.load (std::memory_order_relaxed) ? 0.0f : 1.0f;

    // 落とすのは速く（20 ms）、戻すのはゆっくり（200 ms）。
    const float step = target < monitorSafetyGain ? kSafetyFadeOutStep : kSafetyFadeInStep;

    for (int i = 0; i < numSamples; ++i)
    {
        const float delta = target - monitorSafetyGain;

        monitorSafetyGain += delta > 0.0f ? juce::jmin (step, delta)
                                          : juce::jmax (-step, delta);

        for (int c = 0; c < numChannels; ++c)
            channels[c][i] *= monitorSafetyGain;
    }
}

void AudioEngine::clearMonitorProtection() noexcept
{
    monitorProtection.store (false, std::memory_order_relaxed);
}

//==============================================================================
AudioEngine::BufferProbeResult AudioEngine::probeBestBufferSize (DeviceCatalog& catalog,
                                                                 const juce::String& endpointId,
                                                                 bool allowAsio, bool allowExclusive,
                                                                 double preferredRate)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    BufferProbeResult best;

    const auto* inEntry  = catalog.findInput  (endpointId);
    const auto* outEntry = catalog.findOutput (endpointId);

    if (inEntry == nullptr)
        return best;

    for (const auto backend : makeFallbackLadder (inEntry->bestBackend, allowAsio, allowExclusive))
    {
        auto* type = catalog.getType (backend);

        if (type == nullptr)
            continue;

        const auto inName  = inEntry->getJuceNameFor (backend);
        const auto outName = outEntry != nullptr ? outEntry->getJuceNameFor (backend) : juce::String();

        if (inName.isEmpty())
            continue;

        std::unique_ptr<juce::AudioIODevice> probe (type->createDevice (outName, inName));

        if (probe == nullptr)
            continue;

        const auto sizes = probe->getAvailableBufferSizes();
        probe.reset();

        // 大きすぎるものは試すだけ無駄。小さすぎるものはドロップアウトの元。
        for (const int size : sizes)
        {
            if (size < 32 || size > 256)
                continue;

            std::unique_ptr<juce::AudioIODevice> d (type->createDevice (outName, inName));

            if (d == nullptr)
                continue;

            const auto inMask  = makeChannelMask (d->getInputChannelNames().size(), kMaxLegChannels);
            const auto outMask = makeChannelMask (d->getOutputChannelNames().size(),
                                                  outName.isNotEmpty() ? kMaxLegChannels : 0);

            const double rate = pickSampleRate (*d, preferredRate);

            if (d->open (inMask, outMask, rate, size).isNotEmpty() || ! d->isOpen())
                continue;

            const int total = juce::jmax (0, d->getInputLatencyInSamples())
                            + juce::jmax (0, d->getOutputLatencyInSamples());

            d->close();

            if (total > 0 && (best.bestBlockSize == 0 || total < best.bestTotalLatencySamples))
            {
                best.bestBlockSize = size;
                best.bestTotalLatencySamples = total;
            }
        }

        if (best.bestBlockSize > 0)
            break;   // 最良のバックエンドで見つかったらそれ以上は降りない
    }

    if (best.bestBlockSize > 0)
        best.summaryJapanese = jp ("バッファ ") + juce::String (best.bestBlockSize)
                             + jp (" サンプルが最短でした（機器の申告 ")
                             + juce::String (best.bestTotalLatencySamples) + jp (" サンプル）。");

    return best;
}

//==============================================================================
void AudioEngine::stop()
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    const bool wasRunning = running.load (std::memory_order_acquire) || masterDevice != nullptr;

    teardown();

    {
        const juce::ScopedLock sl (statusLock);
        status = Status{};
    }

    if (wasRunning)
        listeners.call ([] (Listener& l) { l.engineStatusChanged(); });
}

void AudioEngine::teardown()
{
    stopTakePlayback();

    running.store (false, std::memory_order_release);

    // 停止順はマスターが先。先にスレーブを止めるとマスターが行き先の無いリングへ
    // 書き続け、オーバーランカウンタだけが無意味に伸びる。
    if (masterDevice  != nullptr) masterDevice->stop();
    if (monitorDevice != nullptr) monitorDevice->stop();
    if (sendDevice    != nullptr) sendDevice->stop();

    if (masterDevice  != nullptr) masterDevice->close();
    if (monitorDevice != nullptr) monitorDevice->close();
    if (sendDevice    != nullptr) sendDevice->close();

    masterDevice.reset();
    monitorDevice.reset();
    sendDevice.reset();

    monitorCallback.reset();
    sendCallback.reset();

    collapsed.store (false, std::memory_order_relaxed);
    silentInputBlocks.store (0, std::memory_order_relaxed);

    beep.resetPlayback();
    monitorLimiter.reset();
}

void AudioEngine::restartAsync()
{
    triggerAsyncUpdate();
}

void AudioEngine::handleAsyncUpdate()
{
    if (lastRequest.inputEndpointId.isEmpty())
        return;

    // start() が lastRequest を書き換えるので、値でコピーしてから渡す。
    GraphRequest request = lastRequest;

    // restartAsync() の存在理由そのもの。詳細設定は params.noiseQuality だけを書き換えて
    // ここへ来るので、保存済みリクエストの古い値で組み直すと遅延が変わらない。
    request.noiseQuality = params.getNoiseQuality();

    start (request);
}

void AudioEngine::deviceListChanged()
{
    // 使用中のデバイスが消えても自動では選び直さない（DECISIONS.md）。
    // 勝手に別のマイクへ切り替わるほうが、音が出ないことより厄介な事故になる。
    listeners.call ([] (Listener& l) { l.engineDeviceListChanged(); });
}

void AudioEngine::reportError (const juce::String& japaneseMessage)
{
    {
        const juce::ScopedLock sl (statusLock);
        status.lastErrorJapanese = japaneseMessage;
    }

    listeners.call ([&japaneseMessage] (Listener& l) { l.engineErrorOccurred (japaneseMessage); });
}

//==============================================================================
AudioEngine::Status AudioEngine::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

void AudioEngine::setRecorderTap (Recorder* recorder) noexcept
{
    jassert (! isRunning());   // 寿命の競合を避けるため、停止中に一度だけ
    recorderTap.store (recorder, std::memory_order_release);
}

DriftRing::Diagnostics AudioEngine::getSendDiagnostics() const noexcept
{
    return sendRing.getDiagnostics();
}

DriftRing::Diagnostics AudioEngine::getMonitorDiagnostics() const noexcept
{
    return monitorRing.getDiagnostics();
}

bool AudioEngine::hasSendRing() const noexcept
{
    // running を acquire で読んでから見る。running が true の間、これらのポインタは
    // メッセージスレッドからも書き換えられない。
    return running.load (std::memory_order_acquire) && sendDevice != nullptr;
}

bool AudioEngine::hasMonitorRing() const noexcept
{
    return running.load (std::memory_order_acquire) && monitorDevice != nullptr;
}

bool AudioEngine::isSilentInputSuspected() const noexcept
{
    if (! running.load (std::memory_order_acquire))
        return false;

    const double rate = masterSampleRate.load (std::memory_order_relaxed);
    const double threshold = 3.0 * (rate > 0.0 ? rate : kPreferredSampleRate);

    return static_cast<double> (silentInputBlocks.load (std::memory_order_relaxed)) >= threshold;
}

void AudioEngine::addListener (Listener* l)    { listeners.add (l); }
void AudioEngine::removeListener (Listener* l) { listeners.remove (l); }

//==============================================================================
// テイク試聴。モニターバスにしか混ざらない。
//==============================================================================
bool AudioEngine::startTakePlayback (const juce::File& wavFile)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    stopTakePlayback();

    if (! wavFile.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (wavFile));

    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return false;

    const double outRate = masterSampleRate.load (std::memory_order_relaxed);
    const double srcRate = reader->sampleRate > 0.0 ? reader->sampleRate : outRate;

    const auto maxSourceSamples = static_cast<juce::int64> (kMaxTakePlaybackSeconds * srcRate);
    const int  sourceLength = static_cast<int> (juce::jmin (reader->lengthInSamples, maxSourceSamples));

    if (sourceLength <= 0)
        return false;

    // 末尾の 8 サンプルは Lagrange の 5 点カーネルが読み越す分の余白。
    juce::AudioBuffer<float> source (kInternalNumChannels, sourceLength + 8);
    source.clear();
    reader->read (&source, 0, sourceLength, 0, true, false);

    int finalLength = sourceLength;

    if (std::abs (srcRate - outRate) > 1.0 && outRate > 0.0)
    {
        // 録音時と再生時でデバイスが変わった場合。ここで合わせておかないと
        // テイクだけ音程がずれて「ボイチェンが壊れた」に見える。
        const double ratio = srcRate / outRate;
        finalLength = static_cast<int> (static_cast<double> (sourceLength) / ratio);

        if (finalLength <= 0)
            return false;

        juce::AudioBuffer<float> resampled (kInternalNumChannels, finalLength);
        juce::Interpolators::Lagrange interpolator;
        interpolator.process (ratio, source.getReadPointer (0), resampled.getWritePointer (0), finalLength);
        playbackBuffer = std::move (resampled);
    }
    else
    {
        playbackBuffer = std::move (source);
    }

    playbackPos.store (0, std::memory_order_relaxed);
    playbackLengthSamples.store (finalLength, std::memory_order_relaxed);
    playbackActive.store (true, std::memory_order_release);

    return true;
}

void AudioEngine::stopTakePlayback()
{
    playbackActive.store (false, std::memory_order_release);

    // playbackBuffer を差し替える前に、進行中のコールバックが抜けるのを待つ。
    // 待つのはメッセージスレッド側だけ。オーディオスレッドは旗を立てるだけで止まらない。
    for (int i = 0; i < 200 && playbackInCallback.load (std::memory_order_acquire); ++i)
        juce::Thread::sleep (1);

    playbackPos.store (0, std::memory_order_relaxed);
}

bool AudioEngine::isPlayingTake() const noexcept
{
    return playbackActive.load (std::memory_order_acquire);
}

double AudioEngine::getPlaybackPositionSeconds() const noexcept
{
    const double rate = masterSampleRate.load (std::memory_order_relaxed);
    return rate > 0.0 ? playbackPos.load (std::memory_order_relaxed) / rate : 0.0;
}

double AudioEngine::getPlaybackLengthSeconds() const noexcept
{
    const double rate = masterSampleRate.load (std::memory_order_relaxed);
    return rate > 0.0 ? playbackLengthSamples.load (std::memory_order_relaxed) / rate : 0.0;
}

//==============================================================================
// マスターコールバック。DSP が動くのはここだけ。
//==============================================================================
void AudioEngine::MasterCallback::audioDeviceIOCallbackWithContext (
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;

    const int capacity = owner.monoIn.getNumSamples();

    if (numSamples <= 0 || numSamples > capacity)
    {
        // prepare 時の想定を超えるブロックが来た。ここでは確保できないので無音を出す。
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData != nullptr && outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], juce::jmax (0, numSamples));

        return;
    }

    // アトミックを読むのはブロック先頭のこの 1 回だけ。以降はこのコピーしか見ない。
    const VoiceParamSnapshot snap = owner.params.snapshot();

    //--------------------------------------------------------------------------
    // 1. 入力チャンネルをモノラル化
    float* mono = owner.monoIn.getWritePointer (0);
    int mixed = 0;

    const int pick = owner.inputChannelPick.load (std::memory_order_relaxed);

    if (pick >= 0 && pick < numInputChannels
         && inputChannelData != nullptr && inputChannelData[pick] != nullptr)
    {
        // 指定された 1 本だけを使う。無音の隣と平均されて 6 dB 下がるのを避ける。
        juce::FloatVectorOperations::copy (mono, inputChannelData[pick], numSamples);
        mixed = 1;
    }
    else
    {
        for (int ch = 0; ch < numInputChannels; ++ch)
        {
            if (inputChannelData == nullptr || inputChannelData[ch] == nullptr)
                continue;

            if (mixed == 0) juce::FloatVectorOperations::copy (mono, inputChannelData[ch], numSamples);
            else            juce::FloatVectorOperations::add  (mono, inputChannelData[ch], numSamples);

            ++mixed;
        }
    }

    if (mixed == 0)
        juce::FloatVectorOperations::clear (mono, numSamples);
    else if (mixed > 1)
        juce::FloatVectorOperations::multiply (mono, 1.0f / static_cast<float> (mixed), numSamples);

    // 入り口での保険。ドライバから壊れた値が来てもここで止める。
    for (int i = 0; i < numSamples; ++i)
        if (! std::isfinite (mono[i]))
            mono[i] = 0.0f;

    //--------------------------------------------------------------------------
    // 2. 波形・レベルメーターのタップ。DSP より前の「生のマイク」を見せる。
    owner.inputPeaks.pushSamples (mono, numSamples);
    owner.inputRaw.push (mono, numSamples);

    // Windows のマイクプライバシー拒否は「開けるが完全な無音が来る」形で現れる。
    // 微小レベルではなく厳密なゼロで判定するのはそのため。
    bool anyNonZero = false;

    for (int i = 0; i < numSamples; ++i)
    {
        if (mono[i] != 0.0f)
        {
            anyNonZero = true;
            break;
        }
    }

    if (anyNonZero)
    {
        owner.silentInputBlocks.store (0, std::memory_order_relaxed);
    }
    else
    {
        const int accumulated = owner.silentInputBlocks.load (std::memory_order_relaxed);

        if (accumulated < kSilentSampleCap)
            owner.silentInputBlocks.store (accumulated + numSamples, std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    // 3. DSP: HPF → ノイズ除去 → PSOLA → 出力トリム
    float* processedPtr = owner.processed.getWritePointer (0);
    owner.voice.process (mono, processedPtr, numSamples, snap);

    //--------------------------------------------------------------------------
    // 声質判定用のタップ。録音と同じ位置＝変換後・ミュート前・ビープ前。
    // ここに置くと「いま相手に届いている声」がそのまま判定対象になる。
    owner.outputRaw.push (processedPtr, numSamples);

    // 4. 録音タップ。DSP 後・ミュート前・ビープ前 =「相手に聞こえるはずの音」。
    if (auto* recorder = owner.recorderTap.load (std::memory_order_acquire))
    {
        const float* const recordChannels[kInternalNumChannels] = { processedPtr };
        recorder->writeBlock (recordChannels, kInternalNumChannels, numSamples);
    }

    //--------------------------------------------------------------------------
    // 5. 送信バス。
    //
    // ★このブロックには BeepGenerator への参照も、試聴音への参照も 1 つも無い。
    //   ピコ音が相手に届かない保証はこの「コードが存在しないこと」であって、
    //   実行時の if ではない。レビュー時はまずここを確認すること。
    //
    // sendDevice を読んでいるのは、リングが前セッションの prepare 済状態を
    // 引きずるため。停止順（マスターが先）により、このコールバックが走っている間に
    // メッセージスレッドがこのポインタを書き換えることはない。
    if (owner.sendDevice != nullptr)
    {
        if (snap.muted)
        {
            // monoIn はもう用済み。無音源として使い回す（確保しないため）。
            juce::FloatVectorOperations::clear (mono, numSamples);
            const float* const silence[kInternalNumChannels] = { mono };
            owner.sendRing.write (silence, kInternalNumChannels, numSamples);
        }
        else
        {
            const float* const sendSource[kInternalNumChannels] = { processedPtr };
            owner.sendRing.write (sendSource, kInternalNumChannels, numSamples);
        }
    }

    //--------------------------------------------------------------------------
    // 6. モニターバス。送信リングへの書き込みは既に終わっているので、
    //    以降で足すものは構造的に相手へ届かない。
    float* monitorPtr = owner.monitorBus.getWritePointer (0);

    if (snap.muted && snap.muteAlsoSilencesMonitor)
        juce::FloatVectorOperations::clear (monitorPtr, numSamples);
    else
        juce::FloatVectorOperations::copy (monitorPtr, processedPtr, numSamples);

    // 録音テイクの試聴。ミュート設定に関わらず鳴らす（本人が再生を押しているので）。
    if (owner.playbackActive.load (std::memory_order_acquire))
    {
        owner.playbackInCallback.store (true, std::memory_order_release);

        if (owner.playbackActive.load (std::memory_order_acquire))
        {
            const int length = owner.playbackLengthSamples.load (std::memory_order_relaxed);
            int position = owner.playbackPos.load (std::memory_order_relaxed);
            const int available = juce::jlimit (0, numSamples, length - position);

            if (available > 0)
            {
                juce::FloatVectorOperations::add (monitorPtr,
                                                  owner.playbackBuffer.getReadPointer (0) + position,
                                                  available);
                position += available;
                owner.playbackPos.store (position, std::memory_order_relaxed);
            }

            if (position >= length)
                owner.playbackActive.store (false, std::memory_order_release);
        }

        owner.playbackInCallback.store (false, std::memory_order_release);
    }

    float* const monitorChannels[kInternalNumChannels] = { monitorPtr };

    // ビープとリミッタはここ 1 箇所だけ。collapse でもそうでなくても同じ場所なので、
    // BeepGenerator の再生位置が 2 スレッドから触られることはありえない。
    owner.beep.addToMonitor (monitorChannels, kInternalNumChannels, numSamples);
    owner.monitorLimiter.process (monitorChannels, kInternalNumChannels, numSamples);
    owner.applyMonitorSafety (monitorChannels, kInternalNumChannels, numSamples);

    if (owner.collapsed.load (std::memory_order_relaxed))
    {
        // collapse 構成: 同一クロックなのでリングもドリフト補正も無い。
        // モニター遅延は純粋にデバイス遅延だけになる。
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData != nullptr && outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[ch], monitorPtr, numSamples);
    }
    else
    {
        const float* const monitorSource[kInternalNumChannels] = { monitorPtr };
        owner.monitorRing.write (monitorSource, kInternalNumChannels, numSamples);

        // 入力専用で開いているので通常 0 チャンネル。念のため無音にしておく。
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData != nullptr && outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
    }
}

void AudioEngine::MasterCallback::audioDeviceAboutToStart (juce::AudioIODevice*)
{
    // 確保は start() で済ませてある。ここでやることは状態のリセットだけ。
    owner.inputPeaks.resetAccumulator();
    owner.silentInputBlocks.store (0, std::memory_order_relaxed);
}

void AudioEngine::MasterCallback::audioDeviceStopped() {}

void AudioEngine::MasterCallback::audioDeviceError (const juce::String&)
{
    // オーディオスレッドから呼ばれる。ここで juce::String を触ったり Listener を
    // 呼んだりはできないので、メッセージスレッドへ蹴るだけにする。restartAsync() の
    // 中身は PostMessage 1 発なので厳密には RT 安全ではないが、この経路に来た時点で
    // ストリームは既に壊れている。グラフを組み直せば復帰し、駄目なら start() が
    // 日本語のエラーを出して止まる。
    owner.restartAsync();
}

//==============================================================================
// スレーブコールバック。ring.read() 以外は何もしない。
// ★ビープもリミッタもここには無い。モニター用インスタンスにも無い。
//==============================================================================
void AudioEngine::SlaveCallback::audioDeviceIOCallbackWithContext (
    const float* const*, int,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;

    if (outputChannelData == nullptr || numOutputChannels <= 0 || numSamples <= 0)
        return;

    // ドリフト補正のリサンプルはこの中で走る。
    ring.read (outputChannelData, numOutputChannels, numSamples);
}

void AudioEngine::SlaveCallback::audioDeviceAboutToStart (juce::AudioIODevice*) {}
void AudioEngine::SlaveCallback::audioDeviceStopped() {}

void AudioEngine::SlaveCallback::audioDeviceError (const juce::String&)
{
    juce::ignoreUnused (monitorLeg);
    owner.restartAsync();
}

} // namespace kvc
