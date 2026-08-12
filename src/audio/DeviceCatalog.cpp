// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "audio/DeviceCatalog.h"

#include <juce_events/juce_events.h>

#include <limits>

#include <windows.h>
#include <mmdeviceapi.h>

namespace kvc
{

namespace
{

//==============================================================================
/** 日本語リテラル用。juce::String(const char*) は CharPointer_ASCII 経由なので
    127 超のバイトを渡すと jassert で落ちる（ビルドは通る）。 */
inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

//==============================================================================
// Functiondiscoverykeys_devpkey.h の PROPERTYKEY は INITGUID を定義した TU でしか
// 実体化されない。あのヘッダを include して INITGUID を撒くと他の COM GUID まで
// この TU に複製されるので、必要な 3 つだけ値を直に持つ。
// 値は Windows SDK 10.0.17763.0 の同ヘッダと一致（JUCE 自身も同じことをしている）。
const PROPERTYKEY kPkeyDeviceFriendlyName
    = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
const PROPERTYKEY kPkeyDeviceEnumeratorName
    = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 24 };
const PROPERTYKEY kPkeyInterfaceFriendlyName
    = { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } },  2 };

/** ASIO ドライバは MMDevice のエンドポイントではないので合成 ID を振る。
    MMDevice の ID は必ず "{0.0.x.00000000}.{guid}" 形式なので衝突しない。 */
const char* const kAsioIdPrefix = "ASIO:";

//==============================================================================
/** JUCE のメッセージスレッドは OleInitialize 済みだが、それに依存せず自前で数える。
    RPC_E_CHANGED_MODE（既に MTA）のときは失敗扱いにして CoUninitialize を呼ばない。 */
struct ScopedCom
{
    ScopedCom()
        : needsUninit (SUCCEEDED (CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    {
    }

    ~ScopedCom()
    {
        if (needsUninit)
            CoUninitialize();
    }

    const bool needsUninit;

    JUCE_DECLARE_NON_COPYABLE (ScopedCom)
};

//==============================================================================
/** 最小限の COM ポインタ。juce::ComSmartPtr は JUCE_CORE_INCLUDE_COM_SMART_PTR を
    juce_core.h より前に定義した TU でしか見えないので、ここでは自前で持つ。 */
template <typename Type>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() noexcept { reset(); }

    ComPtr (const ComPtr&) = delete;
    ComPtr& operator= (const ComPtr&) = delete;

    Type* get() const noexcept          { return ptr; }
    Type* operator->() const noexcept   { return ptr; }
    explicit operator bool() const noexcept { return ptr != nullptr; }

    Type** resetAndGetAddress() noexcept        { reset(); return &ptr; }
    void** resetAndGetVoidAddress() noexcept    { reset(); return reinterpret_cast<void**> (&ptr); }

    void reset() noexcept
    {
        if (ptr != nullptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
    }

private:
    Type* ptr = nullptr;
};

//==============================================================================
struct EndpointInfo
{
    juce::String id;
    juce::String friendlyName;    ///< PKEY_Device_FriendlyName（生）
    juce::String uniqueName;      ///< JUCE の appendNumbersToDuplicates 適用後
    juce::String interfaceName;   ///< PKEY_DeviceInterface_FriendlyName（アダプタ名）
    juce::String enumeratorName;  ///< "USB" / "HDAUDIO" / "BTHENUM" / "SWD" など
    bool isInput = false;
    bool isVirtual = false;
    bool isBluetooth = false;
    bool isDefaultConsole = false;
    bool isDefaultCommunications = false;
};

//==============================================================================
juce::String propString (IPropertyStore* store, const PROPERTYKEY& key)
{
    if (store == nullptr)
        return {};

    PROPVARIANT value;
    PropVariantInit (&value);

    juce::String result;

    if (SUCCEEDED (store->GetValue (key, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
        result = juce::String (value.pwszVal);

    PropVariantClear (&value);
    return result;
}

juce::String endpointIdOf (IMMDevice* device)
{
    if (device == nullptr)
        return {};

    LPWSTR raw = nullptr;
    juce::String result;

    if (SUCCEEDED (device->GetId (&raw)) && raw != nullptr)
    {
        result = juce::String (raw);
        CoTaskMemFree (raw);
    }

    return result;
}

juce::String defaultEndpointId (IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role)
{
    ComPtr<IMMDevice> device;

    if (enumerator == nullptr
         || FAILED (enumerator->GetDefaultAudioEndpoint (flow, role, device.resetAndGetAddress())))
        return {};

    return endpointIdOf (device.get());
}

//==============================================================================
bool looksVirtual (const juce::String& friendlyName, const juce::String& adapterName)
{
    const auto n = (friendlyName + " " + adapterName).toLowerCase();

    static const char* const needles[] =
    {
        "syncroom", "voicemeeter", "vb-audio", "vb audio", "virtual cable",
        "cable input", "cable output", "krisp", "nvidia broadcast", "rtx voice",
        "oculus", "pico ", "virtual desktop", "stereo mix", "wave out mix",
        "what u hear", "virtual audio", "loopback"
    };

    for (auto* needle : needles)
        if (n.contains (needle))
            return true;

    // 日本語表記のステレオミキサー（表記ゆれが 2 通りある）。
    return n.contains (jp ("ステレオ ミキサー")) || n.contains (jp ("ステレオミキサー"));
}

bool looksBluetooth (const juce::String& friendlyName,
                     const juce::String& adapterName,
                     const juce::String& enumeratorName)
{
    const auto e = enumeratorName.toUpperCase();

    if (e.startsWith ("BTHENUM") || e.startsWith ("BTHHFENUM") || e.startsWith ("BTHLEENUM"))
        return true;

    const auto n = (friendlyName + " " + adapterName).toLowerCase();

    return n.contains ("bluetooth")
        || n.contains ("hands-free")
        || n.contains ("handsfree")
        || n.contains ("airpods")
        || n.contains (jp ("ハンズフリー"));
}

/** 「相手に送る音」候補の優先度。レンダー側にだけ意味がある。 */
int computeSendPriority (const juce::String& friendlyName)
{
    const auto n = friendlyName.toLowerCase();

    if (n.contains ("syncroom"))     return 100;

    if (n.contains ("voicemeeter"))
    {
        if (n.contains ("aux"))      return 70;
        if (n.contains ("vaio3"))    return 65;
        return 80;
    }

    if (n.contains ("cable input") || n.contains ("virtual cable"))  return 60;

    return 0;
}

/** 同じ仮想ケーブルの表裏かどうかを判定するためのキー。
    物理インターフェイスには使わない（MOTU の入力と出力は同一アダプタで当然なため）。 */
juce::String virtualCableFamily (const juce::String& friendlyName)
{
    const auto n = friendlyName.toLowerCase();

    // SYNCROOM は録音側・再生側とも同じ表示名（「ライン (Yamaha SYNCROOM Driver (WDM))」）
    // になり得るので、一般則より先に明示のキーで拾う。
    if (n.contains ("syncroom"))  return "syncroom";

    if (n.contains ("voicemeeter"))
    {
        if (n.contains ("aux"))    return "voicemeeter-aux";
        if (n.contains ("vaio3"))  return "voicemeeter-vaio3";
        return "voicemeeter-vaio";
    }

    if (n.contains ("virtual cable") || n.contains ("cable input") || n.contains ("cable output"))
        return "vb-cable";

    // 未知の仮想ケーブル向けの一般則: 「… Input (アダプタ)」と「… Output (アダプタ)」を
    // 同一系統とみなす。Krisp Microphone / Krisp Speaker のような
    // 「ループバックではない仮想デバイス対」をここで誤検出しないための書き方。
    auto stripped = n.replace ("input", " ")
                     .replace ("output", " ")
                     .replace (jp ("入力"), " ")
                     .replace (jp ("出力"), " ");

    while (stripped.contains ("  "))
        stripped = stripped.replace ("  ", " ");

    return stripped.trim();
}

//==============================================================================
void enumerateEndpoints (juce::Array<EndpointInfo>& inputEndpoints,
                         juce::Array<EndpointInfo>& outputEndpoints)
{
    inputEndpoints.clearQuick();
    outputEndpoints.clearQuick();

    ComPtr<IMMDeviceEnumerator> enumerator;

    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof (IMMDeviceEnumerator), enumerator.resetAndGetVoidAddress())))
        return;

    // JUCE は eMultimedia の既定エンドポイントを一覧の先頭へ寄せる。並び順を
    // 合わせないと juceName との突き合わせが index 一致で取れなくなる。
    const auto defaultRenderMulti  = defaultEndpointId (enumerator.get(), eRender,  eMultimedia);
    const auto defaultCaptureMulti = defaultEndpointId (enumerator.get(), eCapture, eMultimedia);

    const auto defaultRenderConsole  = defaultEndpointId (enumerator.get(), eRender,  eConsole);
    const auto defaultCaptureConsole = defaultEndpointId (enumerator.get(), eCapture, eConsole);
    const auto defaultRenderComms    = defaultEndpointId (enumerator.get(), eRender,  eCommunications);
    const auto defaultCaptureComms   = defaultEndpointId (enumerator.get(), eCapture, eCommunications);

    ComPtr<IMMDeviceCollection> collection;

    if (FAILED (enumerator->EnumAudioEndpoints (eAll, DEVICE_STATE_ACTIVE,
                                                collection.resetAndGetAddress())))
        return;

    UINT32 numDevices = 0;

    if (FAILED (collection->GetCount (&numDevices)))
        return;

    for (UINT32 i = 0; i < numDevices; ++i)
    {
        ComPtr<IMMDevice> device;

        if (FAILED (collection->Item (i, device.resetAndGetAddress())))
            continue;

        DWORD state = 0;

        if (FAILED (device->GetState (&state)) || state != DEVICE_STATE_ACTIVE)
            continue;

        EndpointInfo info;
        info.id = endpointIdOf (device.get());

        if (info.id.isEmpty())
            continue;

        {
            ComPtr<IPropertyStore> props;

            if (FAILED (device->OpenPropertyStore (STGM_READ, props.resetAndGetAddress())))
                continue;

            info.friendlyName   = propString (props.get(), kPkeyDeviceFriendlyName);
            info.interfaceName  = propString (props.get(), kPkeyInterfaceFriendlyName);
            info.enumeratorName = propString (props.get(), kPkeyDeviceEnumeratorName);
        }

        if (info.friendlyName.isEmpty())
            continue;

        EDataFlow flow = eAll;

        {
            ComPtr<IMMEndpoint> endpoint;

            if (FAILED (device->QueryInterface (__uuidof (IMMEndpoint), endpoint.resetAndGetVoidAddress()))
                 || FAILED (endpoint->GetDataFlow (&flow)))
                continue;
        }

        if (flow != eRender && flow != eCapture)
            continue;

        const bool isInput = (flow == eCapture);

        info.isInput     = isInput;
        info.isVirtual   = looksVirtual   (info.friendlyName, info.interfaceName);
        info.isBluetooth = looksBluetooth (info.friendlyName, info.interfaceName, info.enumeratorName);

        info.isDefaultConsole        = info.id == (isInput ? defaultCaptureConsole : defaultRenderConsole);
        info.isDefaultCommunications = info.id == (isInput ? defaultCaptureComms   : defaultRenderComms);

        auto& target       = isInput ? inputEndpoints : outputEndpoints;
        const auto& primary = isInput ? defaultCaptureMulti : defaultRenderMulti;

        target.insert (info.id == primary ? 0 : -1, info);
    }

    // JUCE と同じ重複名の付け替え（"MOTU M Series" → "MOTU M Series (2)" …）。
    // この機械には MOTU M Series のレンダーエンドポイントが 3 つある。
    auto assignUniqueNames = [] (juce::Array<EndpointInfo>& list)
    {
        juce::StringArray names;

        for (const auto& e : list)
            names.add (e.friendlyName);

        names.appendNumbersToDuplicates (false, false);

        for (int i = 0; i < list.size(); ++i)
            list.getReference (i).uniqueName = names[i];
    };

    assignUniqueNames (inputEndpoints);
    assignUniqueNames (outputEndpoints);
}

//==============================================================================
/** JUCE の getDeviceNames() を、エンドポイント ID で作った行に貼り付ける。 */
void attachBackend (Backend backend,
                    const juce::StringArray& juceNames,
                    const juce::Array<EndpointInfo>& endpoints,
                    juce::Array<DeviceEntry>& entries)
{
    jassert (endpoints.size() == entries.size());

    // 通常はここが true になる。JUCE の WASAPI スキャンも eAll / DEVICE_STATE_ACTIVE を
    // 列挙し、eMultimedia の既定を先頭へ寄せ、同名に番号を振る同じ手順なので。
    bool sameOrder = (juceNames.size() == endpoints.size());

    if (sameOrder)
    {
        for (int i = 0; i < juceNames.size(); ++i)
        {
            if (juceNames[i] != endpoints.getReference (i).uniqueName)
            {
                sameOrder = false;
                break;
            }
        }
    }

    for (int j = 0; j < juceNames.size(); ++j)
    {
        const auto& name = juceNames[j];
        int index = -1;

        if (sameOrder)
        {
            index = j;
        }
        else
        {
            // 2 回の列挙の間にデバイスが抜き差しされた場合だけここへ来る。
            for (int i = 0; i < endpoints.size() && index < 0; ++i)
                if (endpoints.getReference (i).uniqueName == name)
                    index = i;

            for (int i = 0; i < endpoints.size() && index < 0; ++i)
                if (endpoints.getReference (i).friendlyName == name)
                    index = i;
        }

        if (index < 0)
            continue;

        auto& entry = entries.getReference (index);

        if (entry.availableBackends.contains (backend))
            continue;

        entry.availableBackends.add (backend);
        entry.juceNamePerBackend.add (name);
    }
}

/** bestBackend を決め、どのバックエンドからも開けない行を落とす。 */
void finaliseEntries (juce::Array<DeviceEntry>& entries)
{
    // 遅延最優先。ASIO が開けなければ AudioEngine::makeFallbackLadder が
    // 必ず WASAPI へ降りるので、ここで欲張っても「音が出ない」にはならない。
    static const Backend preferenceOrder[] =
    {
        Backend::asio, Backend::wasapiLowLatency, Backend::wasapiShared, Backend::wasapiExclusive
    };

    for (int i = entries.size(); --i >= 0;)
    {
        auto& entry = entries.getReference (i);
        entry.bestBackend = Backend::unknown;

        for (auto b : preferenceOrder)
        {
            if (entry.availableBackends.contains (b))
            {
                entry.bestBackend = b;
                break;
            }
        }

        if (entry.bestBackend == Backend::unknown)
        {
            entries.remove (i);
            continue;
        }

        entry.juceName = entry.getJuceNameFor (entry.bestBackend);
    }
}

//==============================================================================
const DeviceEntry* findById (const juce::Array<DeviceEntry>& list, const juce::String& endpointId) noexcept
{
    if (endpointId.isEmpty())
        return nullptr;

    for (const auto& e : list)
        if (e.endpointId == endpointId)
            return &e;

    return nullptr;
}

} // anonymous namespace

//==============================================================================
juce::String backendBadge (Backend b)
{
    switch (b)
    {
        case Backend::wasapiLowLatency:  return jp ("[低遅延]");
        case Backend::wasapiShared:      return jp ("[通常]");
        case Backend::wasapiExclusive:   return jp ("[排他]");
        case Backend::asio:              return "[ASIO]";
        case Backend::unknown:
        default:                         return {};
    }
}

juce::String backendDisplayName (Backend b)
{
    // JUCE の AudioIODeviceType::getTypeName() と一字一句同じ文字列を返す。
    // 情報タブの表示と getType() の照合を同じ値で行えるようにするため。
    switch (b)
    {
        case Backend::wasapiLowLatency:  return "Windows Audio (Low Latency Mode)";
        case Backend::wasapiShared:      return "Windows Audio";
        case Backend::wasapiExclusive:   return "Windows Audio (Exclusive Mode)";
        case Backend::asio:              return "ASIO";
        case Backend::unknown:
        default:                         return jp ("不明");
    }
}

juce::String sendTargetHintJapanese (const DeviceEntry& entry)
{
    if (entry.isInput || entry.sendPriority <= 0)
        return {};

    const auto n = entry.friendlyName.toLowerCase();

    if (n.contains ("syncroom"))
        return jp ("おすすめ：SYNCROOM に送るならこれ");

    if (n.contains ("voicemeeter"))
        return jp ("代替：VoiceMeeter 経由で送る");

    return jp ("代替：仮想ケーブル経由で送る");
}

//==============================================================================
juce::String DeviceEntry::getJuceNameFor (Backend b) const
{
    const auto index = availableBackends.indexOf (b);

    return juce::isPositiveAndBelow (index, juceNamePerBackend.size()) ? juceNamePerBackend[index]
                                                                       : juce::String();
}

//==============================================================================
DeviceCatalog::DeviceCatalog()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // 3 つの WASAPI タイプは同じエンドポイント群を重複列挙する。それでも 3 つとも
    // 所有するのは、AudioEngine が sharedLowLatency → shared のはしごを降りるときに
    // 別タイプの createDevice() を呼ぶ必要があるから。
    typeWasapiLowLatency.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::sharedLowLatency));
    typeWasapiShared.reset     (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    typeWasapiExclusive.reset  (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::exclusive));

    // JUCE_ASIO=0 のビルドでは nullptr が返る。ASIO ドライバ不在でも同じ。
    typeAsio.reset (juce::AudioIODeviceType::createAudioIODeviceType_ASIO());

    for (auto* type : { typeWasapiLowLatency.get(), typeWasapiShared.get(),
                        typeWasapiExclusive.get(), typeAsio.get() })
        if (type != nullptr)
            type->addListener (this);
}

DeviceCatalog::~DeviceCatalog()
{
    for (auto* type : { typeWasapiLowLatency.get(), typeWasapiShared.get(),
                        typeWasapiExclusive.get(), typeAsio.get() })
        if (type != nullptr)
            type->removeListener (this);
}

//==============================================================================
void DeviceCatalog::rescan()
{
    JUCE_ASSERT_MESSAGE_THREAD

    ScopedCom com;

    struct BackendType { Backend backend; juce::AudioIODeviceType* type; };

    const BackendType wasapiTypes[] =
    {
        { Backend::wasapiLowLatency, typeWasapiLowLatency.get() },
        { Backend::wasapiShared,     typeWasapiShared.get() },
        { Backend::wasapiExclusive,  typeWasapiExclusive.get() }
    };

    // MMDevice を読む前に JUCE 側を更新しておく。順序を逆にすると、抜き差しの
    // 直後に JUCE の一覧だけが古いままになり index 一致が取れなくなる。
    for (const auto& w : wasapiTypes)
        if (w.type != nullptr)
            w.type->scanForDevices();

    if (typeAsio != nullptr)
        typeAsio->scanForDevices();

    juce::Array<EndpointInfo> inputEndpoints, outputEndpoints;
    enumerateEndpoints (inputEndpoints, outputEndpoints);

    auto buildEntries = [] (const juce::Array<EndpointInfo>& endpoints, bool isInput)
    {
        juce::Array<DeviceEntry> result;
        result.ensureStorageAllocated (endpoints.size());

        for (const auto& e : endpoints)
        {
            DeviceEntry entry;
            entry.endpointId              = e.id;
            entry.friendlyName            = e.friendlyName;
            entry.deviceName              = e.interfaceName;
            entry.isInput                 = isInput;
            entry.isVirtual               = e.isVirtual;
            entry.isBluetooth             = e.isBluetooth;
            entry.isDefaultConsole        = e.isDefaultConsole;
            entry.isDefaultCommunications = e.isDefaultCommunications;
            entry.sendPriority            = isInput ? 0 : computeSendPriority (e.friendlyName);

            result.add (entry);
        }

        return result;
    };

    inputs  = buildEntries (inputEndpoints,  true);
    outputs = buildEntries (outputEndpoints, false);

    for (const auto& w : wasapiTypes)
    {
        if (w.type == nullptr)
            continue;

        attachBackend (w.backend, w.type->getDeviceNames (true),  inputEndpoints,  inputs);
        attachBackend (w.backend, w.type->getDeviceNames (false), outputEndpoints, outputs);
    }

    finaliseEntries (inputs);
    finaliseEntries (outputs);

    // ASIO は MMDevice のエンドポイントに名寄せできない（ドライバ単位で、しかも
    // 入出力が不可分）。1 ドライバ = 1 行として、入力側と出力側に同じ合成 ID で
    // 並べる。同じ ID になることで isCollapsible() が自然に true になり、
    // AudioEngine が 1 デバイスとして開く ASIO 本来の使い方に一致する。
    if (typeAsio != nullptr)
    {
        for (const auto& name : typeAsio->getDeviceNames (true))
        {
            if (name.isEmpty())
                continue;

            DeviceEntry entry;
            entry.endpointId   = kAsioIdPrefix + name;
            entry.friendlyName = name;
            entry.deviceName   = name;
            entry.juceName     = name;
            entry.bestBackend  = Backend::asio;
            entry.availableBackends.add (Backend::asio);
            entry.juceNamePerBackend.add (name);

            entry.isInput = true;
            inputs.add (entry);

            entry.isInput = false;
            outputs.add (entry);
        }
    }
}

//==============================================================================
const DeviceEntry* DeviceCatalog::findInput (const juce::String& endpointId) const noexcept
{
    return findById (inputs, endpointId);
}

const DeviceEntry* DeviceCatalog::findOutput (const juce::String& endpointId) const noexcept
{
    return findById (outputs, endpointId);
}

const DeviceEntry* DeviceCatalog::getDefaultInput() const noexcept
{
    // 仮想ケーブルの録音側をマイクの既定にすると、非技術者がそのまま起動して
    // 全音量のハウリングに遭う。役割より先に「仮想でないこと」を優先する。
    const DeviceEntry* comms = nullptr;
    const DeviceEntry* console = nullptr;
    const DeviceEntry* firstPhysical = nullptr;
    const DeviceEntry* firstNonVirtual = nullptr;

    for (const auto& e : inputs)
    {
        if (e.isVirtual)
            continue;

        if (firstNonVirtual == nullptr)
            firstNonVirtual = &e;

        if (e.isBluetooth)
            continue;

        if (firstPhysical == nullptr)                            firstPhysical = &e;
        if (e.isDefaultCommunications && comms == nullptr)       comms = &e;
        if (e.isDefaultConsole && console == nullptr)            console = &e;
    }

    if (comms != nullptr)            return comms;
    if (console != nullptr)          return console;
    if (firstPhysical != nullptr)    return firstPhysical;
    if (firstNonVirtual != nullptr)  return firstNonVirtual;

    return inputs.isEmpty() ? nullptr : &inputs.getReference (0);
}

const DeviceEntry* DeviceCatalog::getDefaultMonitorOutput() const noexcept
{
    const DeviceEntry* console = nullptr;
    const DeviceEntry* comms = nullptr;
    const DeviceEntry* firstNonVirtual = nullptr;

    for (const auto& e : outputs)
    {
        if (e.isVirtual)
            continue;

        if (firstNonVirtual == nullptr)                     firstNonVirtual = &e;
        if (e.isDefaultConsole && console == nullptr)       console = &e;
        if (e.isDefaultCommunications && comms == nullptr)  comms = &e;
    }

    if (console != nullptr)          return console;
    if (comms != nullptr)            return comms;
    if (firstNonVirtual != nullptr)  return firstNonVirtual;

    return outputs.isEmpty() ? nullptr : &outputs.getReference (0);
}

const DeviceEntry* DeviceCatalog::getSuggestedSendOutput() const noexcept
{
    const DeviceEntry* best = nullptr;

    for (const auto& e : outputs)
        if (e.sendPriority > 0 && (best == nullptr || e.sendPriority > best->sendPriority))
            best = &e;

    return best;
}

namespace
{
    /** 機器名の比較用に正規化する。"MOTU M Series" と "MOTU M Series (ASIO)" を
        同じ機器とみなしたい一方、"ASIO4ALL v2" を実機と結び付けてはいけない。 */
    juce::String normaliseDeviceName (const juce::String& s)
    {
        auto t = s.toLowerCase();

        for (const char* noise : { "asio", "driver", "wdm", "audio", "device", "(r)", "(tm)" })
            t = t.replace (noise, " ");

        juce::String out;

        for (auto c : t)
            out += (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c)
                                                                  : juce::String (" "));

        return out.trim();
    }

    /** 同じ実機を指していそうか。トークンが 2 つ以上重なれば同一とみなす。
        "motu m series" ↔ "motu m series" は 3 つ、
        "yamaha steinberg usb" ↔ "yamaha steinberg usb" は 3 つ重なる。 */
    bool namesLikelySameDevice (const juce::String& a, const juce::String& b)
    {
        const auto na = normaliseDeviceName (a);
        const auto nb = normaliseDeviceName (b);

        if (na.isEmpty() || nb.isEmpty())
            return false;

        if (na == nb || na.contains (nb) || nb.contains (na))
            return true;

        juce::StringArray ta, tb;
        ta.addTokens (na, " ", "");
        tb.addTokens (nb, " ", "");
        ta.removeEmptyStrings();
        tb.removeEmptyStrings();

        int shared = 0;

        for (const auto& t : ta)
            if (t.length() >= 3 && tb.contains (t))
                ++shared;

        return shared >= 2;
    }

    /** 実機の裏付けが無い、または他アプリと必ず取り合いになる汎用 ASIO ラッパー。 */
    bool isGenericAsioWrapper (const juce::String& name)
    {
        const auto n = name.toLowerCase();

        return n.contains ("asio4all")
            || n.contains ("generic low latency")
            || n.contains ("voicemeeter")
            || n.contains ("directx")
            || n.contains ("multimedia");
    }
}

DeviceCatalog::Recommendation DeviceCatalog::recommendInitialDevices() const
{
    return recommendCollapse ({}, {});
}

DeviceCatalog::Recommendation DeviceCatalog::recommendCollapse (const juce::String& currentInputId,
                                                                const juce::String& currentMonitorId) const
{
    Recommendation r;

    // いま使っている機器の名前。これと一致する ASIO ドライバが最有力。
    juce::StringArray inUseNames;

    for (const auto& id : { currentInputId, currentMonitorId })
    {
        if (id.isEmpty())
            continue;

        if (const auto* e = findById (inputs, id))
            inUseNames.addIfNotAlreadyThere (e->deviceName.isNotEmpty() ? e->deviceName : e->friendlyName);

        if (const auto* e = findById (outputs, id))
            inUseNames.addIfNotAlreadyThere (e->deviceName.isNotEmpty() ? e->deviceName : e->friendlyName);
    }

    const DeviceEntry* best = nullptr;
    int bestScore = std::numeric_limits<int>::min();

    for (const auto& in : inputs)
    {
        if (in.isVirtual)
            continue;

        // 出力側にも同じ endpointId があるものだけが collapse できる。
        if (findById (outputs, in.endpointId) == nullptr)
            continue;

        int score = 0;

        if (in.bestBackend == Backend::asio)
        {
            score += 100;

            // ★実機の裏付けを要求する。ASIO4ALL v2 や Creative のような
            //   「ドライバはあるが機器が無い / 使っていない」ものを掴むと開けずに無音になる。
            if (isGenericAsioWrapper (in.friendlyName))
                score -= 1000;

            bool backedByRealEndpoint = false;

            for (const juce::Array<DeviceEntry>* list : { &inputs, &outputs })
                for (const auto& e : *list)
                    if (e.bestBackend != Backend::asio && ! e.isVirtual
                         && namesLikelySameDevice (e.deviceName, in.friendlyName))
                        backedByRealEndpoint = true;

            if (backedByRealEndpoint)
                score += 500;
            else
                score -= 500;

            // いま実際にモニターしている機器と同じなら最優先。
            for (const auto& used : inUseNames)
                if (namesLikelySameDevice (used, in.friendlyName))
                    score += 2000;
        }

        if (in.isDefaultCommunications) score += 10;
        if (in.isDefaultConsole)        score += 5;

        if (score > bestScore)
        {
            bestScore = score;
            best = &in;
        }
    }

    // 減点しきったものは「まし」ではなく「壊れる」。採用しない。
    if (best != nullptr && bestScore > 0)
    {
        r.inputId   = best->endpointId;
        r.monitorId = best->endpointId;
        r.collapsed = true;
        return r;
    }

    // collapse できないなら既定デバイス同士。同期バッファは避けられない。
    if (const auto* in = getDefaultInput())
        r.inputId = in->endpointId;

    if (const auto* out = getDefaultMonitorOutput())
        r.monitorId = out->endpointId;

    return r;
}

//==============================================================================
juce::AudioIODeviceType* DeviceCatalog::getType (Backend b) const noexcept
{
    switch (b)
    {
        case Backend::wasapiLowLatency:  return typeWasapiLowLatency.get();
        case Backend::wasapiShared:      return typeWasapiShared.get();
        case Backend::wasapiExclusive:   return typeWasapiExclusive.get();
        case Backend::asio:              return typeAsio.get();
        case Backend::unknown:
        default:                         return nullptr;
    }
}

//==============================================================================
bool DeviceCatalog::isCaptureSideOf (const DeviceEntry& input, const DeviceEntry& sendOutput) const
{
    if (! input.isValid() || ! sendOutput.isValid())
        return false;

    if (! input.isInput || sendOutput.isInput)
        return false;

    // 物理インターフェイスは入力も出力も同じアダプタなのが当たり前なので、
    // 仮想デバイス同士のときだけループとみなす。ここを緩めると
    // 「MOTU のマイクと MOTU のヘッドホン」が誤って弾かれる。
    if (! (input.isVirtual && sendOutput.isVirtual))
        return false;

    const auto a = virtualCableFamily (input.friendlyName);
    const auto b = virtualCableFamily (sendOutput.friendlyName);

    return a.isNotEmpty() && a == b;
}

//==============================================================================
RoutingValidation DeviceCatalog::validateRouting (const juce::String& inputId,
                                                  const juce::String& monitorId,
                                                  const juce::String& sendId) const
{
    RoutingValidation result;

    const auto* input   = findInput  (inputId);
    const auto* monitor = findOutput (monitorId);
    const auto* send    = findOutput (sendId);

    if (input == nullptr)
    {
        result.problem = RoutingProblem::inputMissing;
        result.blocksStart = true;
        result.japaneseMessage = inputId.isEmpty()
            ? jp ("マイクが選ばれていません。「① 機器を選ぶ」でマイクを選んでください。")
            : jp ("前に使っていたマイクが見つかりません。つなぎ直すか、「① 機器を選ぶ」で選び直してください。");
        return result;
    }

    if (monitor == nullptr)
    {
        result.problem = RoutingProblem::monitorMissing;
        result.blocksStart = true;
        result.japaneseMessage = monitorId.isEmpty()
            ? jp ("「自分に聞こえる音」の出力先が選ばれていません。ヘッドホンを選んでください。")
            : jp ("前に使っていた「自分に聞こえる音」の出力先が見つかりません。選び直してください。");
        return result;
    }

    if (sendId.isNotEmpty() && send == nullptr)
    {
        result.problem = RoutingProblem::sendMissing;
        result.blocksStart = true;
        result.japaneseMessage
            = jp ("前に使っていた送信先が見つかりません。このままでは相手に音が届きません。"
                  "「相手に送る音」を選び直すか、「送信しない（モニターのみ）」にしてください。");
        return result;
    }

    if (send != nullptr)
    {
        if (inputId == sendId)
        {
            result.problem = RoutingProblem::inputEqualsSend;
            result.blocksStart = true;
            result.japaneseMessage
                = jp ("マイクと「相手に送る音」に同じ機器が選ばれています。音がぐるぐる回って"
                      "大きなハウリングになります。どちらかを別の機器にしてください。");
            return result;
        }

        if (monitorId == sendId)
        {
            result.problem = RoutingProblem::monitorEqualsSend;
            result.blocksStart = true;
            result.japaneseMessage
                = jp ("「自分に聞こえる音」と「相手に送る音」に同じ機器が選ばれています。"
                      "自分の声が二重に聞こえ、ミュートのピコ音まで相手に届いてしまいます。");
            return result;
        }

        if (isCaptureSideOf (*input, *send))
        {
            result.problem = RoutingProblem::inputIsSendLoopback;
            result.blocksStart = true;
            result.japaneseMessage
                = jp ("マイクに、送信先と同じ仮想ケーブルの録音側が選ばれています。"
                      "送った音がそのまま戻ってきて、ヘッドホンに大音量のハウリングが返ります。"
                      "マイクには実際のマイク（例: MOTU M Series）を選んでください。");
            return result;
        }
    }

    if (input->isBluetooth)
    {
        result.problem = RoutingProblem::inputIsBluetooth;
        result.blocksStart = false;
        result.japaneseMessage
            = jp ("Bluetooth のヘッドセットをマイクに選んでいます。通話モードに切り替わって"
                  "音がこもり、遅れも 150 ms ほど増えます。有線のマイクをおすすめします。");
        return result;
    }

    if (send == nullptr)
    {
        result.problem = RoutingProblem::sendNotSelected;
        result.blocksStart = false;
        result.japaneseMessage
            = jp ("「相手に送る音」が「送信しない（モニターのみ）」になっています。"
                  "自分の声は聞こえますが、相手には届きません。");
        return result;
    }

    return result;
}

//==============================================================================
void DeviceCatalog::addListener (Listener* l)     { listeners.add (l); }
void DeviceCatalog::removeListener (Listener* l)  { listeners.remove (l); }

void DeviceCatalog::audioDeviceListChanged()
{
    JUCE_ASSERT_MESSAGE_THREAD

    rescan();

    // 使用中のデバイスが消えていても、ここで勝手に別の機器へ切り替えない
    // （黙って別のマイクに変わるのが一番わかりにくい壊れ方なので）。
    listeners.call ([] (Listener& l) { l.deviceListChanged(); });
}

} // namespace kvc
