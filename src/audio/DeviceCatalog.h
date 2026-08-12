// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

namespace kvc
{

//==============================================================================
enum class Backend
{
    wasapiLowLatency = 0,   ///< "Windows Audio (Low Latency Mode)"  既定
    wasapiShared,           ///< "Windows Audio"                     フォールバック先
    wasapiExclusive,        ///< "Windows Audio (Exclusive Mode)"    詳細設定でのみ
    asio,                   ///< "ASIO"                              詳細設定でのみ
    unknown
};

/** UI のバッジ文字列: [低遅延] / [通常] / [排他] / [ASIO]。 */
juce::String backendBadge (Backend);

/** 詳細設定の情報タブ向けの正式名。 */
juce::String backendDisplayName (Backend);

//==============================================================================
/** 1 エンドポイント 1 行。3 つの WASAPI タイプが同じエンドポイントを重複列挙するので、
    MMDevice のエンドポイント ID で名寄せ・重複排除した結果がこれ。 */
struct DeviceEntry
{
    /** IMMDevice::GetId() の値。設定の保存・復元はこれだけで行う。
        JUCE の表示名は重複時に " (2)" " (3)" が付き、この機械には MOTU M Series の
        エンドポイントが 3 つあるため、名前で保存すると抜き差しで別デバイスを指す。 */
    juce::String endpointId;

    /** PKEY_Device_FriendlyName。UI に出す名前。"Out 1-2" のような端子名になる。 */
    juce::String friendlyName;

    /** PKEY_DeviceInterface_FriendlyName。機器そのものの名前（"MOTU M Series" など）。
        ASIO ドライバ名と突き合わせて「その ASIO は実機が繋がっているか」を判定するのに使う。
        ASIO エントリ自身にはドライバ名がそのまま入る。 */
    juce::String deviceName;

    /** bestBackend のタイプにおける JUCE の getDeviceNames() 上の名前。
        createDevice() に渡すのは必ずこちら。 */
    juce::String juceName;

    /** このエンドポイントで実際に使うべき最良のバックエンド。 */
    Backend bestBackend = Backend::unknown;

    /** 同じエンドポイントを提供する他のバックエンドと、そこでの JUCE 名。 */
    juce::Array<Backend> availableBackends;
    juce::StringArray    juceNamePerBackend;   // availableBackends と同じ並び

    bool isInput = false;
    bool isVirtual = false;             ///< SYNCROOM / VoiceMeeter / CABLE / Krisp / NVIDIA / Oculus / Pico / ステレオミキサー
    bool isBluetooth = false;
    bool isDefaultConsole = false;
    bool isDefaultCommunications = false;

    /** 「相手に送る音」候補としての優先度。大きいほど上。0 = 非候補。
        SYNCROOM Driver > VoiceMeeter Input > VoiceMeeter Aux Input > CABLE Input。 */
    int sendPriority = 0;

    bool isValid() const noexcept { return endpointId.isNotEmpty(); }

    /** このエントリの juceName を指定バックエンドで引く。無ければ空文字。 */
    juce::String getJuceNameFor (Backend) const;
};

/** 「相手に送る音」の候補に添える日本語の一言。候補でなければ空文字。
    SYNCROOM ドライバだけが「おすすめ」で、あとは「代替」。 */
juce::String sendTargetHintJapanese (const DeviceEntry&);

//==============================================================================
enum class RoutingProblem
{
    none = 0,
    inputMissing,           ///< 保存済みの入力が見つからない
    monitorMissing,
    sendMissing,
    sendNotSelected,        ///< 「送信しない（モニターのみ）」は正常なので blocksStart=false
    inputEqualsSend,        ///< 入力 == 送信。即ループ。
    monitorEqualsSend,      ///< モニター == 送信。自分の声が二重に聞こえ、ピコ音も送られる。
    inputIsSendLoopback,    ///< 入力が送信先デバイスのキャプチャ側。全音量ハウリング。
    inputIsBluetooth        ///< HFP に落ちて 8-16 kHz / 150 ms になる。警告のみ。
};

struct RoutingValidation
{
    RoutingProblem problem = RoutingProblem::none;
    juce::String japaneseMessage;   ///< そのまま UI に出せる日本語
    bool blocksStart = false;       ///< true ならエンジンを起動してはいけない
};

//==============================================================================
/** 4 種の AudioIODeviceType の所有・スキャン・名寄せ・重複排除。

    juce::AudioDeviceManager は使わない。AudioIODeviceType / AudioIODevice を
    直接駆動する（AudioDeviceManager は AudioIODevice を 1 個しか持てず、
    3 デバイス構成を表現できないため）。

    sharedLowLatency は IAudioClient3 非対応ドライバでは isOk() が false になり、
    フォールバックせずに作成そのものが失敗する。はしごは AudioEngine 側が自前で書く。

    スレッド契約: 全メソッドがメッセージスレッド専用。
    audioDeviceListChanged() は JUCE がメッセージスレッドへ回してくる。
*/
class DeviceCatalog : private juce::AudioIODeviceType::Listener
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        /** メッセージスレッドで呼ばれる。デバイスの抜き差しなど。
            使用中のデバイスが消えた場合でも自動で切り替えないこと。 */
        virtual void deviceListChanged() = 0;
    };

    DeviceCatalog();
    ~DeviceCatalog() override;

    /** 4 タイプを scanForDevices し、MMDevice 側の情報と突き合わせて作り直す。
        起動時と「機器を再検索」ボタンから。 */
    void rescan();

    const juce::Array<DeviceEntry>& getInputs()  const noexcept { return inputs; }
    const juce::Array<DeviceEntry>& getOutputs() const noexcept { return outputs; }

    const DeviceEntry* findInput  (const juce::String& endpointId) const noexcept;
    const DeviceEntry* findOutput (const juce::String& endpointId) const noexcept;

    /** 初回起動時の既定。eCommunications ロールを優先し、仮想デバイスは避ける。 */
    const DeviceEntry* getDefaultInput() const noexcept;
    /** eConsole ロール。 */
    const DeviceEntry* getDefaultMonitorOutput() const noexcept;
    /** sendPriority が最大のもの。無ければ nullptr（= 未選択のまま UI に促させる）。 */
    const DeviceEntry* getSuggestedSendOutput() const noexcept;

    /** JUCE_ASIO=0 でビルドした場合や ASIO ドライバ不在なら nullptr。 */
    juce::AudioIODeviceType* getType (Backend) const noexcept;

    /** 入力エンドポイントが、指定した出力エンドポイントと同じ仮想ケーブルの
        キャプチャ側かどうか。SYNCROOM WDM / VoiceMeeter などのループを弾く。 */
    bool isCaptureSideOf (const DeviceEntry& input, const DeviceEntry& sendOutput) const;

    /** 起動前の構成チェック。UI はここで blocksStart のものをグレーアウトする。 */
    RoutingValidation validateRouting (const juce::String& inputId,
                                       const juce::String& monitorId,
                                       const juce::String& sendId) const;

    /** 入力とモニターが同一エンドポイントか（collapse 判定）。
        同一クロックなのでドリフト補正もリングも不要になる、既定の推奨構成。 */
    static bool isCollapsible (const juce::String& inputId, const juce::String& monitorId) noexcept
    {
        return inputId.isNotEmpty() && inputId == monitorId;
    }

    /** 初回起動時の推奨構成。入力とモニターが同一デバイスになる組み合わせ（collapse）を
        最優先で探す。成立するとドリフト補正リングが丸ごと不要になり、別デバイス構成に比べて
        20〜30 ms 減る。実測 69 ms のうち 32.7 ms がこのリングだった。
        ASIO は入出力が 1 ドライバなので同一 endpointId になり、自然にここへ嵌まる。
        見つからなければ既定の入力／出力を返す。 */
    struct Recommendation
    {
        juce::String inputId, monitorId;
        bool collapsed = false;
    };

    Recommendation recommendInitialDevices() const;

    /** いま選ばれている機器を踏まえた推奨。現在モニターしている機器と同じ ASIO ドライバを
        最優先で選ぶ。ASIO4ALL や Voicemeeter のような汎用ラッパー、実機の繋がっていない
        ドライバ（Creative など）は強く減点する。これを見ないと「実際には使っていない
        ASIO4ALL が選ばれて何も開けない」という事故が起きる。 */
    Recommendation recommendCollapse (const juce::String& currentInputId,
                                      const juce::String& currentMonitorId) const;

    void addListener (Listener*);
    void removeListener (Listener*);

private:
    void audioDeviceListChanged() override;

    std::unique_ptr<juce::AudioIODeviceType> typeWasapiLowLatency, typeWasapiShared,
                                             typeWasapiExclusive, typeAsio;

    juce::Array<DeviceEntry> inputs, outputs;
    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceCatalog)
};

} // namespace kvc
