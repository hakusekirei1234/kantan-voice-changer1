// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "app/Settings.h"

#include <juce_core/juce_core.h>

namespace kvc
{

namespace
{

//==============================================================================
// キー名。一度出荷したら変えないこと（変えるとユーザーの設定が黙って消える）。
const char* const keyVersion        = "settingsVersion";

const char* const keyInputEndpoint   = "inputEndpointId";
const char* const keyMonitorEndpoint = "monitorEndpointId";
const char* const keySendEndpoint    = "sendEndpointId";

const char* const keyBlockSize   = "bufferSize";
const char* const keySampleRate  = "sampleRate";
const char* const keyAllowAsio   = "allowAsio";
const char* const keyAllowExcl   = "allowExclusive";
const char* const keyFillMode    = "fillMode";

const char* const keyPitchSemis   = "pitchSemis";
const char* const keyFormantSemis = "formantSemis";
const char* const keyPitchOn      = "pitchOn";
const char* const keyFormantOn    = "formantOn";
const char* const keyNoiseOn      = "nrOn";
const char* const keyNoiseStr     = "nrStrength";
const char* const keyNoiseQuality = "nrQuality";
const char* const keyInputGainDb  = "inputGainDb";
const char* const keyOutputGainDb = "outputGainDb";
const char* const keyMuteMonitor  = "monitorWhileMuted";

const char* const keyHotkeyVk    = "hotkeyVk";
const char* const keyHotkeyMods  = "hotkeyMods";
const char* const keyHotkeyMode  = "hotkeyMode";
const char* const keyHotkeyPass  = "hotkeyPassThrough";
const char* const keyBeepOn      = "beepEnabled";
const char* const keyBeepVolume  = "beepVolume";

const char* const keyExportFormat = "exportFormat";
const char* const keyMp3Bitrate   = "mp3Bitrate";
const char* const keyExportFolder = "exportFolder";

const char* const keyWindowBounds = "windowBounds";

//==============================================================================
// 既定値。DECISIONS.md の確定事項をそのまま置く。
constexpr float kDefaultBeepVolume = 0.6f;
constexpr int   kDefaultMp3Bitrate = 192;

/** 既定は V / トグル / パススルーあり（利用者の指定）。

    単独文字キーなので chooseBackend は必ず WH_KEYBOARD_LL を選ぶ。
    RegisterHotKey は「修飾キーあり かつ トグル かつ 非パススルー」のときだけ使われる。

    ★passThrough=true は必須。false にすると V が全アプリで奪われ、
      どこでも「v」が打てなくなる。true なら他アプリにも V が渡るので、
      チャット入力中は「v が入力され、かつミュートも切り替わる」。
      この二択は原理的に避けられないので、害の小さい側を既定にする。

    ★トレードオフ: LL フックが既定で張られるため、無署名 exe が
      Defender / EDR のキーロガー判定に触れる可能性を既定で背負う。
      修飾キー付きに変えれば RegisterHotKey に切り替わり、この risk は消える。 */
HotkeyBinding defaultBinding() noexcept
{
    HotkeyBinding b;
    b.vk = 'V';
    b.mods = HotkeyMods::none;
    b.mode = HotkeyMode::toggle;
    b.passThrough = true;
    return b;
}

juce::PropertiesFile::Options makeOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName     = "settings";
    o.folderName          = "SimpleVoiceChanger";   // ASCII のみ。CP932 のフォルダ名は面倒を呼ぶ。
    o.filenameSuffix      = "xml";
    o.osxLibrarySubFolder = "Application Support";  // Windows では無視されるが未設定は assert を踏む
    o.commonToAllUsers    = false;
    o.ignoreCaseOfKeyNames = false;
    o.doNotSave           = false;
    o.storageFormat       = juce::PropertiesFile::storeAsXML;

    // 自動保存は完全に切る。デバウンスは Settings 側の Timer が持つ。
    o.millisecondsBeforeSaving = -1;

    return o;
}

template <typename EnumType>
EnumType enumFromInt (int raw, int lo, int hi, EnumType fallback) noexcept
{
    if (raw < lo || raw > hi)
        return fallback;

    return static_cast<EnumType> (raw);
}

} // namespace

//==============================================================================
Settings::Settings() = default;

Settings::~Settings()
{
    stopTimer();

    // デストラクタでも取りこぼさない。PropertiesFile 自身も dtor で
    // saveIfNeeded() を呼ぶが、こちらが先に確定させておく。
    if (appProps != nullptr)
        appProps->saveIfNeeded();
}

//==============================================================================
juce::PropertiesFile* Settings::props() const
{
    if (appProps == nullptr)
        return nullptr;

    return appProps->getUserSettings();
}

juce::File Settings::getSettingsFolder() const
{
    return makeOptions().getDefaultFile().getParentDirectory();
}

//==============================================================================
void Settings::load()
{
    const auto options = makeOptions();
    const auto file = options.getDefaultFile();

    firstRun = ! file.existsAsFile();

    appProps = std::make_unique<juce::ApplicationProperties>();
    appProps->setStorageParameters (options);

    auto* p = appProps->getUserSettings();

    // 壊れた XML で起動不能になることが絶対に無いようにする。
    // isValidFile() は「読めなかった」だけを見るので、内容の妥当性は
    // 各 getter 側でクランプして守る。
    if (p == nullptr || ! p->isValidFile())
    {
        appProps->closeFiles();

        if (file.existsAsFile())
        {
            auto bad = file.getSiblingFile ("settings.bad.xml");

            if (bad.existsAsFile())
                bad.deleteFile();

            if (! file.moveFileTo (bad))
                file.deleteFile();
        }

        appProps->setStorageParameters (options);   // 次の getUserSettings() で作り直す
        firstRun = true;
        dirty = false;
        return;
    }

    const int storedVersion = p->getIntValue (keyVersion, 0);

    if (storedVersion != kSettingsVersion)
    {
        // v1 しか存在しないので移行処理は無い。未知（未来）のバージョンでも
        // 読める値だけ使って落とさない。バージョンだけ今の値に揃えておく。
        p->setValue (keyVersion, kSettingsVersion);
        dirty = true;
    }
}

//==============================================================================
void Settings::scheduleSave()
{
    dirty = true;
    startTimer (kSaveDebounceMs);   // 呼ぶたびに再スタート = デバウンス
}

void Settings::saveIfNeeded()
{
    stopTimer();

    if (appProps != nullptr)
        appProps->saveIfNeeded();

    dirty = false;
}

void Settings::timerCallback()
{
    stopTimer();
    saveIfNeeded();
}

void Settings::resetToDefaults()
{
    const auto options = makeOptions();
    const auto file = options.getDefaultFile();

    stopTimer();
    dirty = false;

    if (appProps != nullptr)
    {
        if (auto* p = appProps->getUserSettings())
        {
            p->clear();
            p->setNeedsToBeSaved (false);
        }

        appProps->closeFiles();
    }
    else
    {
        appProps = std::make_unique<juce::ApplicationProperties>();
    }

    file.deleteFile();

    appProps->setStorageParameters (options);
    firstRun = true;
}

//==============================================================================
// デバイス（エンドポイント ID）
//
// 名前ではなく ID で保存する。JUCE は同名デバイスに " (2)" " (3)" を付けるので、
// 名前で保存すると抜き差しのたびに別のデバイスを指す。

juce::String Settings::getInputEndpointId() const
{
    if (auto* p = props()) return p->getValue (keyInputEndpoint);
    return {};
}

juce::String Settings::getMonitorEndpointId() const
{
    if (auto* p = props()) return p->getValue (keyMonitorEndpoint);
    return {};
}

juce::String Settings::getSendEndpointId() const
{
    if (auto* p = props()) return p->getValue (keySendEndpoint);
    return {};
}

void Settings::setInputEndpointId (const juce::String& v)
{
    if (auto* p = props()) { p->setValue (keyInputEndpoint, v); scheduleSave(); }
}

void Settings::setMonitorEndpointId (const juce::String& v)
{
    if (auto* p = props()) { p->setValue (keyMonitorEndpoint, v); scheduleSave(); }
}

void Settings::setSendEndpointId (const juce::String& v)
{
    if (auto* p = props()) { p->setValue (keySendEndpoint, v); scheduleSave(); }
}

//==============================================================================
// オーディオ

int Settings::getRequestedBlockSize() const
{
    if (auto* p = props())
    {
        const int v = p->getIntValue (keyBlockSize, 0);

        if (v == 0)
            return 0;   // 自動

        return juce::jlimit (16, kMaxBlockSize, v);
    }

    return 0;
}

void Settings::setRequestedBlockSize (int v)
{
    if (auto* p = props()) { p->setValue (keyBlockSize, v); scheduleSave(); }
}

double Settings::getRequestedSampleRate() const
{
    if (auto* p = props())
    {
        const double v = p->getDoubleValue (keySampleRate, 0.0);

        if (v <= 0.0)
            return 0.0;   // 自動

        return juce::jlimit (8000.0, 192000.0, v);
    }

    return 0.0;
}

void Settings::setRequestedSampleRate (double v)
{
    if (auto* p = props()) { p->setValue (keySampleRate, v); scheduleSave(); }
}

bool Settings::getAllowAsio() const
{
    // 既定 ON（遅延最優先。利用者の指定）。
    // ASIO を他アプリ（SYNCROOM / Cubase）が掴んでいると開けないが、
    // makeFallbackLadder が必ず WASAPI へ降りるので「音が出ない」にはならない。
    // 降りたことは画面の状態表示に出る。
    if (auto* p = props()) return p->getBoolValue (keyAllowAsio, true);
    return true;
}

void Settings::setAllowAsio (bool v)
{
    if (auto* p = props()) { p->setValue (keyAllowAsio, v); scheduleSave(); }
}

float Settings::getUiScale() const
{
    // 既定 1.15。等倍だと 9〜11 px の補足文字が読めないという指摘があったため。
    if (auto* p = props())
        return juce::jlimit (1.0f, 1.5f, (float) p->getDoubleValue ("uiScale", 1.15));

    return 1.15f;
}

void Settings::setUiScale (float v)
{
    if (auto* p = props()) { p->setValue ("uiScale", juce::jlimit (1.0f, 1.5f, v)); scheduleSave(); }
}

int Settings::getCharacterIntervalMs() const
{
    if (auto* p = props()) return juce::jlimit (300, 5000, p->getIntValue ("charIntervalMs", 1000));
    return 1000;
}

void Settings::setCharacterIntervalMs (int v)
{
    if (auto* p = props()) { p->setValue ("charIntervalMs", juce::jlimit (300, 5000, v)); scheduleSave(); }
}

bool Settings::getCharacterEnabled() const
{
    // 既定 OFF。判定精度がまだ荒いので、任意で有効にしてもらう扱いにする。
    if (auto* p = props()) return p->getBoolValue ("charEnabled", false);
    return false;
}

void Settings::setCharacterEnabled (bool v)
{
    if (auto* p = props()) { p->setValue ("charEnabled", v); scheduleSave(); }
}

int Settings::getInputChannelIndex() const
{
    // 既定 0（先頭チャンネル 1 本）。従来は先頭 2 本の平均だったが、
    // マイクが片側だけの構成でレベルが半分になるため単一チャンネルを既定にする。
    if (auto* p = props()) return p->getIntValue ("inputChannel", 0);
    return 0;
}

void Settings::setInputChannelIndex (int v)
{
    if (auto* p = props()) { p->setValue ("inputChannel", v); scheduleSave(); }
}

Settings::EngineKind Settings::getEngineKind() const
{
    if (auto* p = props())
        return p->getIntValue ("engineKind", 0) == 1 ? EngineKind::plugin : EngineKind::builtin;

    return EngineKind::builtin;
}

void Settings::setEngineKind (EngineKind v)
{
    if (auto* p = props()) { p->setValue ("engineKind", v == EngineKind::plugin ? 1 : 0); scheduleSave(); }
}

juce::String Settings::getPluginIdentifier() const
{
    if (auto* p = props()) return p->getValue ("pluginId");
    return {};
}

void Settings::setPluginIdentifier (const juce::String& v)
{
    if (auto* p = props()) { p->setValue ("pluginId", v); scheduleSave(); }
}

juce::MemoryBlock Settings::getPluginState() const
{
    juce::MemoryBlock block;

    if (auto* p = props())
    {
        const auto encoded = p->getValue ("pluginState");

        if (encoded.isNotEmpty())
            block.fromBase64Encoding (encoded);
    }

    return block;
}

void Settings::setPluginState (const juce::MemoryBlock& block)
{
    if (auto* p = props())
    {
        p->setValue ("pluginState", block.getSize() > 0 ? block.toBase64Encoding() : juce::String());
        scheduleSave();
    }
}

bool Settings::getAllowExclusive() const
{
    if (auto* p = props()) return p->getBoolValue (keyAllowExcl, false);
    return false;
}

void Settings::setAllowExclusive (bool v)
{
    if (auto* p = props()) { p->setValue (keyAllowExcl, v); scheduleSave(); }
}

DriftRing::FillMode Settings::getFillMode() const
{
    // 既定を lowLatency に。stable は targetFill = m + 2s + 64 で、
    // 別デバイス構成だと単独で 30ms 以上を足していた（実測 69ms の主因）。
    if (auto* p = props())
        return enumFromInt (p->getIntValue (keyFillMode, 1), 0, 1, DriftRing::FillMode::lowLatency);

    return DriftRing::FillMode::lowLatency;
}

void Settings::setFillMode (DriftRing::FillMode v)
{
    if (auto* p = props()) { p->setValue (keyFillMode, static_cast<int> (v)); scheduleSave(); }
}

//==============================================================================
// 声

float Settings::getPitchSemitones() const
{
    if (auto* p = props())
        return juce::jlimit (-12.0f, 12.0f, static_cast<float> (p->getDoubleValue (keyPitchSemis, 0.0)));

    return 0.0f;
}

void Settings::setPitchSemitones (float v)
{
    if (auto* p = props()) { p->setValue (keyPitchSemis, static_cast<double> (v)); scheduleSave(); }
}

float Settings::getFormantSemitones() const
{
    if (auto* p = props())
        return juce::jlimit (-12.0f, 12.0f, static_cast<float> (p->getDoubleValue (keyFormantSemis, 0.0)));

    return 0.0f;
}

void Settings::setFormantSemitones (float v)
{
    if (auto* p = props()) { p->setValue (keyFormantSemis, static_cast<double> (v)); scheduleSave(); }
}

bool Settings::getPitchEnabled() const
{
    if (auto* p = props()) return p->getBoolValue (keyPitchOn, false);
    return false;
}

void Settings::setPitchEnabled (bool v)
{
    if (auto* p = props()) { p->setValue (keyPitchOn, v); scheduleSave(); }
}

bool Settings::getFormantEnabled() const
{
    if (auto* p = props()) return p->getBoolValue (keyFormantOn, false);
    return false;
}

void Settings::setFormantEnabled (bool v)
{
    if (auto* p = props()) { p->setValue (keyFormantOn, v); scheduleSave(); }
}

bool Settings::getNoiseSuppressionEnabled() const
{
    if (auto* p = props()) return p->getBoolValue (keyNoiseOn, false);
    return false;
}

void Settings::setNoiseSuppressionEnabled (bool v)
{
    if (auto* p = props()) { p->setValue (keyNoiseOn, v); scheduleSave(); }
}

NoiseStrength Settings::getNoiseStrength() const
{
    if (auto* p = props())
        return enumFromInt (p->getIntValue (keyNoiseStr, static_cast<int> (NoiseStrength::normal)),
                            0, 2, NoiseStrength::normal);

    return NoiseStrength::normal;
}

void Settings::setNoiseStrength (NoiseStrength v)
{
    if (auto* p = props()) { p->setValue (keyNoiseStr, static_cast<int> (v)); scheduleSave(); }
}

NoiseQuality Settings::getNoiseQuality() const
{
    if (auto* p = props())
        return enumFromInt (p->getIntValue (keyNoiseQuality, static_cast<int> (NoiseQuality::lowLatency256)),
                            0, 1, NoiseQuality::lowLatency256);

    return NoiseQuality::lowLatency256;
}

void Settings::setNoiseQuality (NoiseQuality v)
{
    // これはホットパラメータではない。変更後は AudioEngine::restartAsync() が必須。
    if (auto* p = props()) { p->setValue (keyNoiseQuality, static_cast<int> (v)); scheduleSave(); }
}

float Settings::getInputGainDb() const
{
    if (auto* p = props())
        return juce::jlimit (-24.0f, 24.0f, static_cast<float> (p->getDoubleValue (keyInputGainDb, 0.0)));

    return 0.0f;
}

void Settings::setInputGainDb (float v)
{
    if (auto* p = props()) { p->setValue (keyInputGainDb, static_cast<double> (v)); scheduleSave(); }
}

float Settings::getOutputGainDb() const
{
    if (auto* p = props())
        return juce::jlimit (-24.0f, 24.0f, static_cast<float> (p->getDoubleValue (keyOutputGainDb, 0.0)));

    return 0.0f;
}

void Settings::setOutputGainDb (float v)
{
    if (auto* p = props()) { p->setValue (keyOutputGainDb, static_cast<double> (v)); scheduleSave(); }
}

bool Settings::getMuteAlsoSilencesMonitor() const
{
    // 既定 ON。ミュート中はループバックも切る（相手にも自分にも声を出さない）。
    // ピコ音だけは鳴らす。無音になったのか壊れたのか区別できなくなるため。
    if (auto* p = props()) return p->getBoolValue (keyMuteMonitor, true);
    return true;
}

void Settings::setMuteAlsoSilencesMonitor (bool v)
{
    if (auto* p = props()) { p->setValue (keyMuteMonitor, v); scheduleSave(); }
}

//==============================================================================
// ホットキー・通知音

HotkeyBinding Settings::getHotkeyBinding() const
{
    auto b = defaultBinding();

    if (auto* p = props())
    {
        const int vk = p->getIntValue (keyHotkeyVk, static_cast<int> (b.vk));

        // 0 や範囲外が入っていたら既定へ戻す。壊れた設定でキーが死ぬのを防ぐ。
        if (vk > 0 && vk <= 0xff)
            b.vk = static_cast<uint32_t> (vk);

        const int mods = p->getIntValue (keyHotkeyMods, static_cast<int> (b.mods));
        b.mods = static_cast<uint32_t> (mods) & (HotkeyMods::alt | HotkeyMods::control
                                                  | HotkeyMods::shift | HotkeyMods::win);

        b.mode = enumFromInt (p->getIntValue (keyHotkeyMode, static_cast<int> (b.mode)),
                              0, 1, HotkeyMode::toggle);

        b.passThrough = p->getBoolValue (keyHotkeyPass, b.passThrough);
    }

    // 単独キーは必ずパススルーさせる。ここを落とすと、そのキーが
    // Windows 全体で使えなくなる（ユーザーには「キーボードが壊れた」に見える）。
    if (b.mods == HotkeyMods::none)
        b.passThrough = true;

    return b;
}

void Settings::setHotkeyBinding (const HotkeyBinding& b)
{
    if (auto* p = props())
    {
        p->setValue (keyHotkeyVk,   static_cast<int> (b.vk));
        p->setValue (keyHotkeyMods, static_cast<int> (b.mods));
        p->setValue (keyHotkeyMode, static_cast<int> (b.mode));
        p->setValue (keyHotkeyPass, b.passThrough);
        scheduleSave();
    }
}

bool Settings::getBeepEnabled() const
{
    if (auto* p = props()) return p->getBoolValue (keyBeepOn, true);
    return true;
}

void Settings::setBeepEnabled (bool v)
{
    if (auto* p = props()) { p->setValue (keyBeepOn, v); scheduleSave(); }
}

float Settings::getBeepVolume() const
{
    if (auto* p = props())
        return juce::jlimit (0.0f, 1.0f,
                             static_cast<float> (p->getDoubleValue (keyBeepVolume, kDefaultBeepVolume)));

    return kDefaultBeepVolume;
}

void Settings::setBeepVolume (float v)
{
    if (auto* p = props()) { p->setValue (keyBeepVolume, static_cast<double> (v)); scheduleSave(); }
}

//==============================================================================
// 書き出し

Mp3Exporter::Format Settings::getExportFormat() const
{
    if (auto* p = props())
        return enumFromInt (p->getIntValue (keyExportFormat, static_cast<int> (Mp3Exporter::Format::mp3)),
                            0, 1, Mp3Exporter::Format::mp3);

    return Mp3Exporter::Format::mp3;
}

void Settings::setExportFormat (Mp3Exporter::Format v)
{
    if (auto* p = props()) { p->setValue (keyExportFormat, static_cast<int> (v)); scheduleSave(); }
}

int Settings::getMp3BitrateKbps() const
{
    if (auto* p = props())
    {
        const int v = p->getIntValue (keyMp3Bitrate, kDefaultMp3Bitrate);

        if (v == 128 || v == 192 || v == 320)
            return v;
    }

    return kDefaultMp3Bitrate;
}

void Settings::setMp3BitrateKbps (int v)
{
    if (auto* p = props()) { p->setValue (keyMp3Bitrate, v); scheduleSave(); }
}

juce::File Settings::getExportFolder() const
{
    if (auto* p = props())
    {
        const auto stored = p->getValue (keyExportFolder);

        if (stored.isNotEmpty() && juce::File::isAbsolutePath (stored))
        {
            const juce::File f (stored);

            if (f.isDirectory() || f.getParentDirectory().isDirectory())
                return f;
        }
    }

    return Mp3Exporter::getDefaultExportFolder();
}

void Settings::setExportFolder (const juce::File& f)
{
    if (auto* p = props()) { p->setValue (keyExportFolder, f.getFullPathName()); scheduleSave(); }
}

//==============================================================================
// ウィンドウ

juce::Rectangle<int> Settings::getWindowBounds() const
{
    if (auto* p = props())
    {
        const auto s = p->getValue (keyWindowBounds);

        if (s.isNotEmpty())
            return juce::Rectangle<int>::fromString (s);
    }

    return {};
}

void Settings::setWindowBounds (juce::Rectangle<int> r)
{
    if (r.isEmpty())
        return;

    if (auto* p = props())
    {
        const auto s = r.toString();

        // ウィンドウ移動中は 1 秒に何十回も呼ばれる。値が変わっていなければ
        // デバウンスタイマーを叩き直さない。
        if (p->getValue (keyWindowBounds) == s)
            return;

        p->setValue (keyWindowBounds, s);
        scheduleSave();
    }
}

//==============================================================================
void Settings::applyToParams (VoiceParams& params) const
{
    params.pitchSemitones.store   (getPitchSemitones(),   std::memory_order_relaxed);
    params.formantSemitones.store (getFormantSemitones(), std::memory_order_relaxed);
    params.pitchEnabled.store     (getPitchEnabled(),     std::memory_order_relaxed);
    params.formantEnabled.store   (getFormantEnabled(),   std::memory_order_relaxed);

    params.noiseSuppressionEnabled.store (getNoiseSuppressionEnabled(), std::memory_order_relaxed);
    params.noiseStrength.store (static_cast<int> (getNoiseStrength()), std::memory_order_relaxed);
    params.noiseQuality.store  (static_cast<int> (getNoiseQuality()),  std::memory_order_relaxed);

    params.inputGainDb.store  (getInputGainDb(),  std::memory_order_relaxed);
    params.outputGainDb.store (getOutputGainDb(), std::memory_order_relaxed);

    params.muteAlsoSilencesMonitor.store (getMuteAlsoSilencesMonitor(), std::memory_order_relaxed);

    params.beepEnabled.store (getBeepEnabled(), std::memory_order_relaxed);
    params.beepVolume.store  (getBeepVolume(),  std::memory_order_relaxed);

    // ミュート状態は保存しない。起動直後に自分の声が相手へ届かないより、
    // 「起動したら送信されている」の方が予測しやすい。
    params.muted.store (false, std::memory_order_relaxed);
}

void Settings::captureFromParams (const VoiceParams& params)
{
    setPitchSemitones   (params.pitchSemitones.load   (std::memory_order_relaxed));
    setFormantSemitones (params.formantSemitones.load (std::memory_order_relaxed));
    setPitchEnabled     (params.pitchEnabled.load     (std::memory_order_relaxed));
    setFormantEnabled   (params.formantEnabled.load   (std::memory_order_relaxed));

    setNoiseSuppressionEnabled (params.noiseSuppressionEnabled.load (std::memory_order_relaxed));
    setNoiseStrength (static_cast<NoiseStrength> (params.noiseStrength.load (std::memory_order_relaxed)));
    setNoiseQuality  (params.getNoiseQuality());

    setInputGainDb  (params.inputGainDb.load  (std::memory_order_relaxed));
    setOutputGainDb (params.outputGainDb.load (std::memory_order_relaxed));

    setMuteAlsoSilencesMonitor (params.muteAlsoSilencesMonitor.load (std::memory_order_relaxed));

    setBeepEnabled (params.beepEnabled.load (std::memory_order_relaxed));
    setBeepVolume  (params.beepVolume.load  (std::memory_order_relaxed));
}

} // namespace kvc
