// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "audio/PluginHost.h"

#if KVC_VST2_ENABLED
 #include "audio/Vst2Host.h"
#endif

namespace kvc
{

namespace
{
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    const char* const kVst3Prefix = "VST3:";
    const char* const kVst2Prefix = "VST2:";

    const char* const kListFileName    = "plugins.xml";
    const char* const kBlockedFileName = "plugins_blocked.txt";
    const char* const kOpeningFileName = "plugin_opening.txt";
    const char* const kDeadFileName    = "plugins_dead.txt";

    /** VST2 の標準フォルダ。VST3 と違い規約が緩いので、実際に使われている場所を並べる。 */
    juce::Array<juce::File> vst2SearchPaths()
    {
        juce::Array<juce::File> paths;

        const auto add = [&paths] (const juce::String& p)
        {
            const juce::File f (p);
            if (f.isDirectory())
                paths.addIfNotAlreadyThere (f);
        };

        const auto programFiles = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory)
                                      .getFullPathName();

        add (programFiles + "\\Common Files\\VST2");
        add (programFiles + "\\Common Files\\Steinberg\\VST2");
        add (programFiles + "\\VSTPlugins");
        add (programFiles + "\\Steinberg\\VSTPlugins");
        add ("C:\\VSTPlugins");

        return paths;
    }

    //==========================================================================
    /** JUCE の AudioPluginInstance を ExternalProcessor に被せる。 */
    class Vst3LoadedPlugin final : public LoadedPlugin
    {
    public:
        Vst3LoadedPlugin (std::unique_ptr<juce::AudioPluginInstance> i, juce::String displayName)
            : instance (std::move (i)), name (std::move (displayName)) {}

        ~Vst3LoadedPlugin() override
        {
            if (instance != nullptr)
                instance->releaseResources();
        }

        void prepare (double sampleRate, int maxBlockSize) override
        {
            if (instance == nullptr)
                return;

            // まずモノラル構成を試す。通ればチャンネル数ぶんの無駄が消える。
            {
                auto layout = instance->getBusesLayout();
                const auto mono = juce::AudioChannelSet::mono();

                for (auto& b : layout.inputBuses)  b = mono;
                for (auto& b : layout.outputBuses) b = mono;

                instance->setBusesLayout (layout);   // 失敗しても既定構成のまま続行する
            }

            instance->setNonRealtime (false);
            instance->prepareToPlay (sampleRate, maxBlockSize);

            numIn  = juce::jmax (0, instance->getTotalNumInputChannels());
            numOut = juce::jmax (0, instance->getTotalNumOutputChannels());

            const int channels = juce::jmax (1, numIn, numOut);

            // process 中は確保できないので、ここで最大ブロック長ぶん確保しきる。
            scratch.setSize (channels, juce::jmax (1, maxBlockSize), false, true, false);
            scratch.clear();
            midi.ensureSize (256);

            latency = juce::jmax (0, instance->getLatencySamples());
        }

        void reset() override
        {
            if (instance != nullptr)
                instance->reset();

            scratch.clear();
        }

        void processMono (float* buffer, int numSamples) noexcept override
        {
            if (instance == nullptr || buffer == nullptr || numSamples <= 0)
                return;

            const int n = juce::jmin (numSamples, scratch.getNumSamples());

            if (n <= 0)
                return;

            // モノラル入力を全入力チャンネルへ複製する。
            for (int ch = 0; ch < scratch.getNumChannels(); ++ch)
                juce::FloatVectorOperations::copy (scratch.getWritePointer (ch), buffer, n);

            juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(),
                                           scratch.getNumChannels(), 0, n);

            midi.clear();
            instance->processBlock (view, midi);

            juce::FloatVectorOperations::copy (buffer, scratch.getReadPointer (0), n);

            // prepare より長いブロックが来た場合の余りは無音にする（確保はできない）。
            for (int i = n; i < numSamples; ++i)
                buffer[i] = 0.0f;
        }

        int getLatencySamples() const noexcept override { return latency; }
        juce::String getName() const override           { return name; }

        bool hasEditor() const override
        {
            return instance != nullptr && instance->hasEditor();
        }

        juce::Component* createEditor() override
        {
            if (instance == nullptr || ! instance->hasEditor())
                return nullptr;

            return instance->createEditorAndMakeActive();
        }

        juce::MemoryBlock saveState() override
        {
            juce::MemoryBlock block;

            if (instance != nullptr)
                instance->getStateInformation (block);

            return block;
        }

        void restoreState (const juce::MemoryBlock& block) override
        {
            if (instance != nullptr && block.getSize() > 0)
                instance->setStateInformation (block.getData(), static_cast<int> (block.getSize()));
        }

        int findParameterIndexByKeywords (const juce::StringArray& keywords) const override
        {
            if (instance == nullptr)
                return -1;

            const auto& params = instance->getParameters();

            for (int i = 0; i < params.size(); ++i)
            {
                const auto n = params[i]->getName (64).toLowerCase();

                for (const auto& k : keywords)
                    if (n.contains (k.toLowerCase()))
                        return i;
            }

            return -1;
        }

        void setParameterNormalised (int index, float value) override
        {
            if (instance == nullptr)
                return;

            const auto& params = instance->getParameters();

            if (juce::isPositiveAndBelow (index, params.size()))
                params[index]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value));
        }

        float getParameterNormalised (int index) const override
        {
            if (instance == nullptr)
                return 0.0f;

            const auto& params = instance->getParameters();

            return juce::isPositiveAndBelow (index, params.size()) ? params[index]->getValue() : 0.0f;
        }

        juce::String getParameterText (int index) const override
        {
            if (instance == nullptr)
                return {};

            const auto& params = instance->getParameters();

            return juce::isPositiveAndBelow (index, params.size())
                       ? params[index]->getCurrentValueAsText() : juce::String();
        }

    private:
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::String name;
        juce::AudioBuffer<float> scratch;
        juce::MidiBuffer midi;
        int numIn = 0, numOut = 0, latency = 0;
    };
}

//==============================================================================
PluginHost::PluginHost (const juce::File& stateFolder)
    : folder (stateFolder)
{
    folder.createDirectory();

    // JUCE 8 では AudioPluginFormatManager::addDefaultFormats() が削除されている。
    // そもそも VST3 しか要らないので明示的に足す。
    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());

    loadPersistedState();

    // 前回「これから開く」の印を残したまま落ちていたら、その識別子を除外する。
    const auto crashed = readOpeningMark();

    if (crashed.isNotEmpty())
    {
        blocked.addIfNotAlreadyThere (crashed);
        clearOpeningMark();
        savePersistedState();
    }
}

PluginHost::~PluginHost() = default;

//==============================================================================
void PluginHost::loadPersistedState()
{
    if (auto xml = juce::XmlDocument::parse (folder.getChildFile (kListFileName)))
        knownList.recreateFromXml (*xml);

    const auto blockedFile = folder.getChildFile (kBlockedFileName);

    if (blockedFile.existsAsFile())
        blocked.addLines (blockedFile.loadFileAsString());

    blocked.removeEmptyStrings();
    blocked.removeDuplicates (false);
}

void PluginHost::savePersistedState() const
{
    if (auto xml = knownList.createXml())
        xml->writeTo (folder.getChildFile (kListFileName));

    folder.getChildFile (kBlockedFileName).replaceWithText (blocked.joinIntoString ("\n"));
}

void PluginHost::markOpening (const juce::String& identifier)
{
    folder.getChildFile (kOpeningFileName).replaceWithText (identifier);
}

void PluginHost::clearOpeningMark()
{
    folder.getChildFile (kOpeningFileName).deleteFile();
}

juce::String PluginHost::readOpeningMark() const
{
    const auto f = folder.getChildFile (kOpeningFileName);
    return f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
}

void PluginHost::unblock (const juce::String& identifier)
{
    blocked.removeString (identifier);
    savePersistedState();
}

//==============================================================================
void PluginHost::scan()
{
    scanVst3();

   #if KVC_VST2_ENABLED
    scanVst2();
   #endif

    savePersistedState();
}

void PluginHost::scanVst3()
{
    for (auto* format : formatManager.getFormats())
    {
        if (format == nullptr || format->getName() != "VST3")
            continue;

        // dead man's pedal: 走査中に落ちたファイルを記録し、次回は飛ばす。
        juce::PluginDirectoryScanner scanner (knownList, *format,
                                              format->getDefaultLocationsToSearch(),
                                              true,
                                              folder.getChildFile (kDeadFileName),
                                              false);

        juce::String nameBeingScanned;

        while (scanner.scanNextFile (true, nameBeingScanned)) {}
    }

    entries.clearQuick();

    for (const auto& d : knownList.getTypes())
    {
        Entry e;
        e.identifier   = kVst3Prefix + d.createIdentifierString();
        e.name         = d.name;
        e.manufacturer = d.manufacturerName;
        e.format       = "VST3";
        e.path         = d.fileOrIdentifier;

        if (! blocked.contains (e.identifier))
            entries.add (e);
    }
}

void PluginHost::scanVst2()
{
   #if KVC_VST2_ENABLED
    // VST2 は読み込まずに列挙だけする。DllMain を走らせずに済ませたいので、
    // 妥当性の確認は load() のときに行う（そこには落ちても復帰できる印がある）。
    for (const auto& dir : vst2SearchPaths())
    {
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, true, "*.dll"))
        {
            Entry e;
            e.identifier   = kVst2Prefix + f.getFullPathName();
            e.name         = f.getFileNameWithoutExtension();
            e.manufacturer = {};
            e.format       = "VST2";
            e.path         = f.getFullPathName();

            if (! blocked.contains (e.identifier))
                entries.add (e);
        }
    }
   #endif
}

bool PluginHost::addFromFile (const juce::File& file, juce::String& errorJapanese)
{
    if (! file.exists())
    {
        errorJapanese = jp ("ファイルが見つかりません。");
        return false;
    }

    const auto ext = file.getFileExtension().toLowerCase();

    if (ext == ".vst3")
    {
        for (auto* format : formatManager.getFormats())
        {
            if (format == nullptr || format->getName() != "VST3")
                continue;

            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile (found, file.getFullPathName());

            if (found.isEmpty())
            {
                errorJapanese = jp ("VST3 プラグインとして読み込めませんでした。");
                return false;
            }

            for (auto* d : found)
                knownList.addType (*d);

            scanVst3();
            savePersistedState();
            return true;
        }
    }

   #if KVC_VST2_ENABLED
    if (ext == ".dll")
    {
        Entry e;
        e.identifier = kVst2Prefix + file.getFullPathName();
        e.name       = file.getFileNameWithoutExtension();
        e.format     = "VST2";
        e.path       = file.getFullPathName();

        if (findEntry (e.identifier) == nullptr)
            entries.add (e);

        return true;
    }
   #endif

    errorJapanese = jp ("対応していない形式です。.vst3 を選んでください。");
    return false;
}

const PluginHost::Entry* PluginHost::findEntry (const juce::String& identifier) const
{
    for (const auto& e : entries)
        if (e.identifier == identifier)
            return &e;

    return nullptr;
}

//==============================================================================
std::unique_ptr<LoadedPlugin> PluginHost::load (const juce::String& identifier,
                                                double sampleRate, int maxBlockSize,
                                                juce::String& errorJapanese)
{
    if (identifier.isEmpty())
    {
        errorJapanese = jp ("プラグインが選ばれていません。");
        return {};
    }

    if (blocked.contains (identifier))
    {
        errorJapanese = jp ("このプラグインは前回の起動時にアプリを巻き込んで落ちたため、"
                            "自動的に無効にしています。詳細設定から解除できます。");
        return {};
    }

    // プラグインは同一プロセスで動く。落ちたら次回起動時にここで弾けるよう印を残す。
    markOpening (identifier);

    std::unique_ptr<LoadedPlugin> result;

    if (identifier.startsWith (kVst3Prefix))
    {
        const auto wanted = identifier.fromFirstOccurrenceOf (kVst3Prefix, false, false);

        juce::PluginDescription desc;
        bool found = false;

        for (const auto& d : knownList.getTypes())
        {
            if (d.createIdentifierString() == wanted)
            {
                desc = d;
                found = true;
                break;
            }
        }

        if (! found)
        {
            clearOpeningMark();
            errorJapanese = jp ("プラグインが見つかりません。「プラグインを再検索」を押してください。");
            return {};
        }

        juce::String err;
        auto instance = formatManager.createPluginInstance (desc, sampleRate, maxBlockSize, err);

        if (instance == nullptr)
        {
            clearOpeningMark();
            errorJapanese = jp ("プラグインを読み込めませんでした: ") + err;
            return {};
        }

        result = std::make_unique<Vst3LoadedPlugin> (std::move (instance), desc.name);
    }
   #if KVC_VST2_ENABLED
    else if (identifier.startsWith (kVst2Prefix))
    {
        const juce::File file (identifier.fromFirstOccurrenceOf (kVst2Prefix, false, false));
        result = createVst2Plugin (file, errorJapanese);
    }
   #endif
    else
    {
        clearOpeningMark();
        errorJapanese = jp ("対応していないプラグイン形式です。");
        return {};
    }

    if (result == nullptr)
    {
        clearOpeningMark();

        if (errorJapanese.isEmpty())
            errorJapanese = jp ("プラグインを読み込めませんでした。");

        return {};
    }

    result->prepare (sampleRate, maxBlockSize);

    // ここまで来たら少なくとも読み込みと prepare は生き延びた。
    clearOpeningMark();

    return result;
}

//==============================================================================
juce::StringArray PluginHost::pitchKeywords()
{
    // Pitchproof は "Pitch"、Graillon は "Pitch Shift"、élastique は "Transpose"。
    return { "pitch", "transpose", "semitone", "shift", "key" };
}

juce::StringArray PluginHost::formantKeywords()
{
    return { "formant", "gender", "timbre", "gender/formant" };
}

} // namespace kvc
