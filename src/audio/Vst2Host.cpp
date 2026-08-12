// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "audio/Vst2Host.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <windows.h>

#include <cstdint>

//==============================================================================
// VST2 の ABI 定義。
//
// ★これは Steinberg の VST2 SDK ヘッダ（pluginterfaces/vst2.x/aeffect.h）ではない。
//   あのヘッダは配布が終了しており、新規ライセンスも発行されていないため使えない。
//   ここに置いてあるのは、公開されている VST 2.4 の ABI（構造体レイアウトと
//   オペコード番号）を自前で書き起こしたもので、Ardour / LMMS などが長年使ってきた
//   のと同じやり方。Steinberg のコードは 1 行も含まない。
//
//   Pitchproof が VST2 でしか配布されていないため必要になった。
//   公開配布版は -DKVC_ENABLE_VST2=OFF でこのファイルごと外すこと。
//
// レイアウトは自然アラインメントに依存している。x64 では magic(int32) の後に
// 4 バイトのパディングが入り、以降のポインタが 8 バイト境界に乗る。これは
// 本家ヘッダと同じ配置になる。ここを間違えると読み込んだ瞬間に落ちる。
//==============================================================================

namespace
{

struct AEffect;

using AEffectDispatcherProc   = intptr_t (*) (AEffect*, int32_t, int32_t, intptr_t, void*, float);
using AEffectProcessProc      = void (*) (AEffect*, float**, float**, int32_t);
using AEffectProcessDoubleProc= void (*) (AEffect*, double**, double**, int32_t);
using AEffectSetParameterProc = void (*) (AEffect*, int32_t, float);
using AEffectGetParameterProc = float (*) (AEffect*, int32_t);
using AudioMasterProc         = intptr_t (*) (AEffect*, int32_t, int32_t, intptr_t, void*, float);

struct AEffect
{
    int32_t                  magic;
    AEffectDispatcherProc    dispatcher;
    AEffectProcessProc       process;
    AEffectSetParameterProc  setParameter;
    AEffectGetParameterProc  getParameter;
    int32_t                  numPrograms;
    int32_t                  numParams;
    int32_t                  numInputs;
    int32_t                  numOutputs;
    int32_t                  flags;
    intptr_t                 resvd1;          // ホストが自由に使ってよい枠
    intptr_t                 resvd2;
    int32_t                  initialDelay;    // 申告される遅延（サンプル）
    int32_t                  realQualities;
    int32_t                  offQualities;
    float                    ioRatio;
    void*                    object;
    void*                    user;
    int32_t                  uniqueID;
    int32_t                  version;
    AEffectProcessProc       processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char                     future[56];
};

constexpr int32_t kEffectMagic = 0x56737450;   // 'VstP'

// effect opcodes
enum
{
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
    effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19,
    effGetChunk = 23, effSetChunk = 24,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effCanDo = 51, effGetVstVersion = 58, effSetProcessPrecision = 77
};

// audioMaster opcodes
enum
{
    amAutomate = 0, amVersion = 1, amCurrentId = 2, amIdle = 3,
    amGetTime = 7, amIOChanged = 13, amSizeWindow = 15,
    amGetSampleRate = 16, amGetBlockSize = 17,
    amGetCurrentProcessLevel = 23,
    amGetVendorString = 32, amGetProductString = 33, amGetVendorVersion = 34,
    amCanDo = 37, amUpdateDisplay = 42, amBeginEdit = 43, amEndEdit = 44
};

enum
{
    effFlagsHasEditor     = 1 << 0,
    effFlagsCanReplacing  = 1 << 4,
    effFlagsProgramChunks = 1 << 5
};

struct ERect { int16_t top, left, bottom, right; };

struct VstTimeInfo
{
    double  samplePos, sampleRate, nanoSeconds, ppqPos, tempo,
            barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset,
            smpteFrameRate, samplesToNextClock, flags;
};

//==============================================================================
inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

class Vst2Plugin;

/** VSTPluginMain の実行中はまだ effect->resvd1 を書けないのに、プラグインが
    audioMaster を呼んでくる。その間だけ「いま構築中のホスト」をここで指す。 */
thread_local Vst2Plugin* g_constructing = nullptr;

/** ディスパッチャ経由の文字列取得。VST2 の文字列は最大 64 バイト、非 UTF-8。 */
juce::String dispatchString (AEffect* e, int32_t opcode, int32_t index = 0)
{
    if (e == nullptr || e->dispatcher == nullptr)
        return {};

    char buffer[128] = {};
    e->dispatcher (e, opcode, index, 0, buffer, 0.0f);
    buffer[sizeof (buffer) - 1] = 0;

    // VST2 の文字列はコードページ不定。ASCII 以外はまず出てこないので
    // CP932 として解釈しておく（化けても表示だけの影響）。
    return juce::String::createStringFromData (buffer, (int) strlen (buffer));
}

//==============================================================================
/** プラグインのエディタ用コンテナ HWND。
    effEditOpen には「親にできる HWND」を渡す必要がある。JUCE の
    HWNDComponent は与えた HWND を自分の下へ付け替えてくれるので、
    こちらで空の子ウィンドウを 1 枚作って渡す。 */
HWND createEditorContainer()
{
    static bool registered = false;
    static const wchar_t* const className = L"KvcVst2EditorContainer";

    auto* instance = GetModuleHandleW (nullptr);

    if (! registered)
    {
        WNDCLASSEXW wc {};
        wc.cbSize        = sizeof (wc);
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = instance;
        wc.lpszClassName = className;
        // IDC_ARROW は UNICODE 未定義時 LPSTR に化けるが、実体は整数アトムなので
        // ワイド版へキャストして渡すのが定石。
        wc.hCursor       = LoadCursorW (nullptr, reinterpret_cast<LPCWSTR> (IDC_ARROW));

        RegisterClassExW (&wc);
        registered = true;
    }

    return CreateWindowExW (0, className, L"", WS_CHILD | WS_VISIBLE,
                            0, 0, 100, 100,
                            GetDesktopWindow(), nullptr, instance, nullptr);
}

//==============================================================================
class Vst2Plugin final : public kvc::LoadedPlugin
{
public:
    Vst2Plugin() = default;

    ~Vst2Plugin() override
    {
        closeEditor();

        if (effect != nullptr)
        {
            if (resumed)
                effect->dispatcher (effect, effMainsChanged, 0, 0, nullptr, 0.0f);

            effect->dispatcher (effect, effClose, 0, 0, nullptr, 0.0f);
            effect = nullptr;
        }

        if (module != nullptr)
            FreeLibrary (module);
    }

    bool open (const juce::File& dll, juce::String& errorJapanese)
    {
        module = LoadLibraryW (dll.getFullPathName().toWideCharPointer());

        if (module == nullptr)
        {
            errorJapanese = jp ("DLL を読み込めませんでした（32bit 版のプラグインではありませんか？）。");
            return false;
        }

        auto entry = reinterpret_cast<AEffect* (*) (AudioMasterProc)> (
                         GetProcAddress (module, "VSTPluginMain"));

        if (entry == nullptr)
            entry = reinterpret_cast<AEffect* (*) (AudioMasterProc)> (GetProcAddress (module, "main"));

        if (entry == nullptr)
        {
            errorJapanese = jp ("VST2 プラグインではありません。");
            return false;
        }

        g_constructing = this;
        effect = entry (&audioMaster);
        g_constructing = nullptr;

        if (effect == nullptr || effect->magic != kEffectMagic)
        {
            effect = nullptr;
            errorJapanese = jp ("VST2 プラグインの初期化に失敗しました。");
            return false;
        }

        effect->resvd1 = reinterpret_cast<intptr_t> (this);

        effect->dispatcher (effect, effOpen, 0, 0, nullptr, 0.0f);

        displayName = dispatchString (effect, effGetEffectName);

        if (displayName.isEmpty())
            displayName = dll.getFileNameWithoutExtension();

        if ((effect->flags & effFlagsCanReplacing) == 0 && effect->process == nullptr)
        {
            errorJapanese = jp ("このプラグインは音声処理に対応していません。");
            return false;
        }

        return true;
    }

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize) override
    {
        if (effect == nullptr)
            return;

        rate = sampleRate;
        blockSize = juce::jmax (1, maxBlockSize);

        if (resumed)
        {
            effect->dispatcher (effect, effMainsChanged, 0, 0, nullptr, 0.0f);
            resumed = false;
        }

        effect->dispatcher (effect, effSetSampleRate, 0, 0, nullptr, (float) sampleRate);
        effect->dispatcher (effect, effSetBlockSize, 0, (intptr_t) blockSize, nullptr, 0.0f);
        effect->dispatcher (effect, effSetProcessPrecision, 0, 0 /* 32bit */, nullptr, 0.0f);

        numIn  = juce::jmax (0, effect->numInputs);
        numOut = juce::jmax (0, effect->numOutputs);

        const int channels = juce::jmax (1, numIn, numOut);

        // process 中に確保はできない。ここで最大長ぶん確保しきる。
        inBuffer.setSize (channels, blockSize, false, true, false);
        outBuffer.setSize (channels, blockSize, false, true, false);
        inBuffer.clear();
        outBuffer.clear();

        inPtrs.resize ((size_t) channels);
        outPtrs.resize ((size_t) channels);

        for (int ch = 0; ch < channels; ++ch)
        {
            inPtrs[(size_t) ch]  = inBuffer.getWritePointer (ch);
            outPtrs[(size_t) ch] = outBuffer.getWritePointer (ch);
        }

        effect->dispatcher (effect, effMainsChanged, 0, 1, nullptr, 0.0f);
        resumed = true;

        latency = juce::jmax (0, effect->initialDelay);
    }

    void reset() override
    {
        inBuffer.clear();
        outBuffer.clear();
    }

    void processMono (float* buffer, int numSamples) noexcept override
    {
        if (effect == nullptr || buffer == nullptr || numSamples <= 0 || inPtrs.empty())
            return;

        const int n = juce::jmin (numSamples, blockSize);

        if (n <= 0)
            return;

        for (size_t ch = 0; ch < inPtrs.size(); ++ch)
            juce::FloatVectorOperations::copy (inPtrs[ch], buffer, n);

        if ((effect->flags & effFlagsCanReplacing) != 0 && effect->processReplacing != nullptr)
        {
            effect->processReplacing (effect, inPtrs.data(), outPtrs.data(), n);
        }
        else if (effect->process != nullptr)
        {
            // 旧 process は「加算」なので、呼ぶ前に出力を消す必要がある。
            for (size_t ch = 0; ch < outPtrs.size(); ++ch)
                juce::FloatVectorOperations::clear (outPtrs[ch], n);

            effect->process (effect, inPtrs.data(), outPtrs.data(), n);
        }
        else
        {
            return;
        }

        juce::FloatVectorOperations::copy (buffer, outPtrs[0], n);

        for (int i = n; i < numSamples; ++i)
            buffer[i] = 0.0f;
    }

    int getLatencySamples() const noexcept override { return latency; }
    juce::String getName() const override           { return displayName; }

    //==========================================================================
    bool hasEditor() const override
    {
        return effect != nullptr && (effect->flags & effFlagsHasEditor) != 0;
    }

    juce::Component* createEditor() override
    {
        if (! hasEditor())
            return nullptr;

        closeEditor();

        editorContainer = createEditorContainer();

        if (editorContainer == nullptr)
            return nullptr;

        effect->dispatcher (effect, effEditOpen, 0, 0, editorContainer, 0.0f);

        int w = 400, h = 300;

        ERect* r = nullptr;
        effect->dispatcher (effect, effEditGetRect, 0, 0, &r, 0.0f);

        if (r != nullptr)
        {
            w = juce::jmax (64, r->right - r->left);
            h = juce::jmax (64, r->bottom - r->top);
        }

        SetWindowPos (editorContainer, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

        auto holder = std::make_unique<juce::HWNDComponent>();
        holder->setHWND (editorContainer);
        holder->setSize (w, h);

        editorOpen = true;
        return holder.release();
    }

    void closeEditor()
    {
        if (! editorOpen || effect == nullptr)
            return;

        effect->dispatcher (effect, effEditClose, 0, 0, nullptr, 0.0f);
        editorOpen = false;
        editorContainer = nullptr;   // HWNDComponent が破棄済み
    }

    //==========================================================================
    juce::MemoryBlock saveState() override
    {
        juce::MemoryBlock block;

        if (effect == nullptr)
            return block;

        if ((effect->flags & effFlagsProgramChunks) != 0)
        {
            void* data = nullptr;
            const auto size = effect->dispatcher (effect, effGetChunk, 0, 0, &data, 0.0f);

            if (data != nullptr && size > 0)
                block.append (data, (size_t) size);

            return block;
        }

        // チャンク非対応のプラグインはパラメータを並べて保存する。
        for (int i = 0; i < effect->numParams; ++i)
        {
            const float v = effect->getParameter != nullptr ? effect->getParameter (effect, i) : 0.0f;
            block.append (&v, sizeof (v));
        }

        return block;
    }

    void restoreState (const juce::MemoryBlock& block) override
    {
        if (effect == nullptr || block.getSize() == 0)
            return;

        if ((effect->flags & effFlagsProgramChunks) != 0)
        {
            effect->dispatcher (effect, effSetChunk, 0, (intptr_t) block.getSize(),
                                const_cast<void*> (block.getData()), 0.0f);
            return;
        }

        const int count = juce::jmin (effect->numParams,
                                      (int) (block.getSize() / sizeof (float)));

        const auto* values = static_cast<const float*> (block.getData());

        for (int i = 0; i < count; ++i)
            if (effect->setParameter != nullptr)
                effect->setParameter (effect, i, values[i]);
    }

    //==========================================================================
    int findParameterIndexByKeywords (const juce::StringArray& keywords) const override
    {
        if (effect == nullptr)
            return -1;

        for (int i = 0; i < effect->numParams; ++i)
        {
            const auto n = dispatchString (const_cast<AEffect*> (effect), effGetParamName, i).toLowerCase();

            for (const auto& k : keywords)
                if (n.contains (k.toLowerCase()))
                    return i;
        }

        return -1;
    }

    void setParameterNormalised (int index, float value) override
    {
        if (effect != nullptr && effect->setParameter != nullptr
             && juce::isPositiveAndBelow (index, effect->numParams))
            effect->setParameter (effect, index, juce::jlimit (0.0f, 1.0f, value));
    }

    float getParameterNormalised (int index) const override
    {
        if (effect != nullptr && effect->getParameter != nullptr
             && juce::isPositiveAndBelow (index, effect->numParams))
            return effect->getParameter (effect, index);

        return 0.0f;
    }

    juce::String getParameterText (int index) const override
    {
        if (effect == nullptr || ! juce::isPositiveAndBelow (index, effect->numParams))
            return {};

        auto* e = const_cast<AEffect*> (effect);
        return dispatchString (e, effGetParamDisplay, index).trim()
                 + " " + dispatchString (e, effGetParamLabel, index).trim();
    }

private:
    //==========================================================================
    static intptr_t audioMaster (AEffect* e, int32_t opcode, int32_t index,
                                 intptr_t value, void* ptr, float opt)
    {
        juce::ignoreUnused (index, value, opt);

        auto* self = e != nullptr && e->resvd1 != 0
                         ? reinterpret_cast<Vst2Plugin*> (e->resvd1)
                         : g_constructing;

        switch (opcode)
        {
            case amVersion:              return 2400;
            case amCurrentId:            return e != nullptr ? e->uniqueID : 0;
            case amGetCurrentProcessLevel: return 2;   // realtime

            case amGetSampleRate:
                return self != nullptr ? (intptr_t) self->rate : 48000;

            case amGetBlockSize:
                return self != nullptr ? (intptr_t) self->blockSize : 512;

            case amGetTime:
                if (self != nullptr)
                {
                    // 使わないプラグインが大半だが、呼ばれて nullptr を返すと
                    // 落ちる作りのものがあるので必ず有効な構造体を返す。
                    self->timeInfo = {};
                    self->timeInfo.sampleRate = self->rate;
                    self->timeInfo.tempo = 120.0;
                    self->timeInfo.timeSigNumerator = 4;
                    self->timeInfo.timeSigDenominator = 4;
                    return reinterpret_cast<intptr_t> (&self->timeInfo);
                }
                return 0;

            case amGetVendorString:
                if (ptr != nullptr) juce::String ("KantanVoice").copyToUTF8 (static_cast<char*> (ptr), 64);
                return 1;

            case amGetProductString:
                if (ptr != nullptr) juce::String ("KantanVoiceChanger").copyToUTF8 (static_cast<char*> (ptr), 64);
                return 1;

            case amGetVendorVersion:     return 1000;

            case amCanDo:
                // sendVstEvents / sizeWindow などは一切対応しない。
                return 0;

            case amAutomate:
            case amBeginEdit:
            case amEndEdit:
            case amUpdateDisplay:
            case amIOChanged:
            case amIdle:
            case amSizeWindow:
            default:
                return 0;
        }
    }

    HMODULE  module = nullptr;
    AEffect* effect = nullptr;

    juce::String displayName;

    double rate = 48000.0;
    int    blockSize = 512;
    int    numIn = 0, numOut = 0, latency = 0;
    bool   resumed = false;

    juce::AudioBuffer<float> inBuffer, outBuffer;
    std::vector<float*> inPtrs, outPtrs;

    VstTimeInfo timeInfo {};

    HWND editorContainer = nullptr;
    bool editorOpen = false;
};

} // namespace

//==============================================================================
namespace kvc
{

std::unique_ptr<LoadedPlugin> createVst2Plugin (const juce::File& dll, juce::String& errorJapanese)
{
    auto p = std::make_unique<Vst2Plugin>();

    if (! p->open (dll, errorJapanese))
        return {};

    return p;
}

} // namespace kvc
