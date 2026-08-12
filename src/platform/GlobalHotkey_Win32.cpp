// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "platform/GlobalHotkey_Win32.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>

#ifndef MOD_NOREPEAT
 #define MOD_NOREPEAT 0x4000
#endif

#ifndef MAPVK_VK_TO_VSC_EX
 #define MAPVK_VK_TO_VSC_EX 4
#endif

namespace kvc
{

namespace
{

//==============================================================================
// ヘッダの HotkeyMods は windows.h を持ち込まないための再定義なので、
// 値がずれていないことをここで一度だけ確かめる。ずれると RegisterHotKey が
// 「別の修飾キー」で登録され、原因不明の「効かない」報告になる。
static_assert (HotkeyMods::alt     == MOD_ALT,     "HotkeyMods::alt != MOD_ALT");
static_assert (HotkeyMods::control == MOD_CONTROL, "HotkeyMods::control != MOD_CONTROL");
static_assert (HotkeyMods::shift   == MOD_SHIFT,   "HotkeyMods::shift != MOD_SHIFT");
static_assert (HotkeyMods::win     == MOD_WIN,     "HotkeyMods::win != MOD_WIN");
static_assert (SafeKeys::vkNonConvert == VK_NONCONVERT, "vkNonConvert != VK_NONCONVERT");
static_assert (SafeKeys::vkConvert    == VK_CONVERT,    "vkConvert != VK_CONVERT");

constexpr UINT kMsgHotkeyEvent  = WM_APP + 0x51;
constexpr UINT kMsgCaptureEvent = WM_APP + 0x52;

constexpr int  kHotkeyId = 1;              // アプリ用 ID は 0x0000..0xBFFF
constexpr int  kPollIntervalMs = 250;      // 押しっぱなし照合
constexpr int  kWatchdogTicks = 20;        // 250ms * 20 = 5 秒
constexpr int  kCaptureSafetyMs = 15000;   // 保険。ダイアログ側の 5 秒より必ず後

const wchar_t* const kHotkeyWindowClass  = L"KvcHotkeyMessageWindow";
const wchar_t* const kCaptureWindowClass = L"KvcHotkeyCaptureWindow";

juce::String jstr (const char* utf8) { return juce::String::fromUTF8 (utf8); }

//==============================================================================
bool registerWindowClassOnce (const wchar_t* className, WNDPROC proc)
{
    auto* const instance = GetModuleHandleW (nullptr);

    WNDCLASSEXW existing {};
    existing.cbSize = sizeof (existing);

    if (GetClassInfoExW (instance, className, &existing))
        return true;

    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof (wc);
    wc.lpfnWndProc   = proc;
    wc.hInstance     = instance;
    wc.lpszClassName = className;

    return RegisterClassExW (&wc) != 0;
}

/** HWND_MESSAGE のメッセージ専用ウィンドウ。
    RegisterHotKey(nullptr, ...) はスレッドメッセージ（hwnd==NULL）を投げるが、
    JUCE のループは DispatchMessage で捨てるので必ず実体の HWND が要る。 */
HWND createMessageWindow (const wchar_t* className, WNDPROC proc, void* userData)
{
    if (! registerWindowClassOnce (className, proc))
        return nullptr;

    auto* const instance = GetModuleHandleW (nullptr);

    HWND hwnd = CreateWindowExW (0, className, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, instance, nullptr);

    if (hwnd != nullptr)
        SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (userData));

    return hwnd;
}

void destroyMessageWindow (HWND& hwnd)
{
    if (hwnd != nullptr)
    {
        SetWindowLongPtrW (hwnd, GWLP_USERDATA, 0);
        DestroyWindow (hwnd);
        hwnd = nullptr;
    }
}

//==============================================================================
bool keyIsPhysicallyDown (int vk) noexcept
{
    return (GetAsyncKeyState (vk) & 0x8000) != 0;
}

bool isModifierVk (uint32_t vk) noexcept
{
    switch (vk)
    {
        case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU:    case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:    case VK_RWIN:
            return true;
        default:
            return false;
    }
}

uint32_t currentModifierMask() noexcept
{
    uint32_t m = 0;

    if (keyIsPhysicallyDown (VK_CONTROL)) m |= HotkeyMods::control;
    if (keyIsPhysicallyDown (VK_SHIFT))   m |= HotkeyMods::shift;
    if (keyIsPhysicallyDown (VK_MENU))    m |= HotkeyMods::alt;
    if (keyIsPhysicallyDown (VK_LWIN) || keyIsPhysicallyDown (VK_RWIN)) m |= HotkeyMods::win;

    return m;
}

/** 完全一致で判定する。要求されていない修飾キーが押されていたら不一致とする
    （Ctrl+Shift+Alt+M で Ctrl+Shift+M が誤爆しない）。自前で修飾キーの状態を
    追跡するとフォーカス変化でずれるので、毎回 GetAsyncKeyState を引く。 */
bool modifiersMatchNow (uint32_t required) noexcept
{
    return currentModifierMask() == required;
}

//==============================================================================
/** LL フックプロシージャが触れる唯一の状態。
    フックは LowLevelHooksTimeout（既定 300 ms）を超えると Windows に黙って
    切られるので、ここに積んで PostMessage するところまでしかやらない。 */
struct HookShared
{
    static constexpr uint32_t kQueueSize = 64;   // 2 のべき乗
    static constexpr uint8_t  kEventDown = 1;
    static constexpr uint8_t  kEventUp   = 2;

    std::atomic<HWND>     window { nullptr };
    std::atomic<uint32_t> vk { 0 };
    std::atomic<uint32_t> mods { 0 };
    std::atomic<bool>     passThrough { true };
    std::atomic<bool>     keyHeld { false };

    std::atomic<uint32_t> writeIndex { 0 };
    std::atomic<uint8_t>  events[kQueueSize] {};

    uint32_t readIndex = 0;   // メッセージスレッド専有
};

HookShared g_hook;
HHOOK      g_hookHandle = nullptr;   // メッセージスレッド専有

void pushHookEvent (uint8_t kind) noexcept
{
    const uint32_t w = g_hook.writeIndex.load (std::memory_order_relaxed);
    g_hook.events[w & (HookShared::kQueueSize - 1)].store (kind, std::memory_order_relaxed);
    g_hook.writeIndex.store (w + 1, std::memory_order_release);
}

LRESULT CALLBACK lowLevelKeyboardProc (int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*> (lParam);

        if (k != nullptr && (k->flags & LLKHF_INJECTED) == 0)
        {
            const uint32_t boundVk = g_hook.vk.load (std::memory_order_relaxed);

            if (boundVk != 0 && k->vkCode == boundVk)
            {
                const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
                const bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

                const bool modsOk  = modifiersMatchNow (g_hook.mods.load (std::memory_order_relaxed));
                const bool wasHeld = g_hook.keyHeld.load (std::memory_order_relaxed);

                uint8_t kind = 0;

                if (down && modsOk && ! wasHeld)
                {
                    // MOD_NOREPEAT 相当。これが無いと押しっぱなしでキーリピートし、
                    // ミュートがリピート速度で明滅する。
                    g_hook.keyHeld.store (true, std::memory_order_relaxed);
                    kind = HookShared::kEventDown;
                }
                else if (up && wasHeld)
                {
                    // キーアップでは修飾キーの一致を要求しない。Ctrl を先に離されると
                    // 一致しなくなり、押しっぱなし状態から復帰できなくなる。
                    g_hook.keyHeld.store (false, std::memory_order_relaxed);
                    kind = HookShared::kEventUp;
                }

                if (kind != 0)
                {
                    pushHookEvent (kind);

                    if (HWND w = g_hook.window.load (std::memory_order_relaxed))
                        PostMessageW (w, kMsgHotkeyEvent, 0, 0);
                }

                if (! g_hook.passThrough.load (std::memory_order_relaxed)
                     && (modsOk || kind == HookShared::kEventUp))
                    return 1;
            }
        }
    }

    return CallNextHookEx (nullptr, nCode, wParam, lParam);
}

//==============================================================================
/** 割当ダイアログ用の一時フック。本番フックとは完全に別の状態を持つ。 */
struct CaptureShared
{
    std::atomic<HWND>     window { nullptr };
    std::atomic<uint32_t> vk { 0 };
    std::atomic<uint32_t> mods { 0 };
    std::atomic<bool>     cancelled { false };
    std::atomic<bool>     pending { false };
};

CaptureShared g_capture;
HHOOK         g_captureHook = nullptr;   // メッセージスレッド専有

LRESULT CALLBACK captureKeyboardProc (int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*> (lParam);

        if (k != nullptr && (k->flags & LLKHF_INJECTED) == 0)
        {
            // 修飾キーだけは消費せずに通す。LL フックで握り潰したキーは
            // GetAsyncKeyState にも反映されなくなり、直後の修飾キー判定が壊れる。
            if (! isModifierVk (k->vkCode))
            {
                const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

                if (down)
                {
                    if (k->vkCode == VK_ESCAPE)
                        g_capture.cancelled.store (true, std::memory_order_relaxed);
                    else
                    {
                        g_capture.vk.store (k->vkCode, std::memory_order_relaxed);
                        g_capture.mods.store (currentModifierMask(), std::memory_order_relaxed);
                    }

                    if (! g_capture.pending.exchange (true, std::memory_order_acq_rel))
                    {
                        if (HWND w = g_capture.window.load (std::memory_order_relaxed))
                            PostMessageW (w, kMsgCaptureEvent, 0, 0);
                    }
                }

                // キャプチャ中の打鍵は他アプリへ漏らさない。
                return 1;
            }
        }
    }

    return CallNextHookEx (nullptr, nCode, wParam, lParam);
}

} // namespace

//==============================================================================
//  GlobalHotkey::Impl
//==============================================================================
struct GlobalHotkey::Impl
{
    explicit Impl (GlobalHotkey& o) : owner (o) {}

    ~Impl()
    {
        removeHook();
        removeRegisteredHotkey();
        destroyMessageWindow (window);
    }

    //==========================================================================
    bool createWindow()
    {
        if (window == nullptr)
            window = createMessageWindow (kHotkeyWindowClass, &Impl::wndProc, this);

        return window != nullptr;
    }

    bool addRegisteredHotkey (const HotkeyBinding& b, DWORD& errOut)
    {
        // MOD_NOREPEAT は必須。無いと押しっぱなしでミュートが 30 Hz で明滅する。
        if (RegisterHotKey (window, kHotkeyId, b.mods | MOD_NOREPEAT, static_cast<UINT> (b.vk)))
        {
            hotkeyRegistered = true;
            return true;
        }

        errOut = GetLastError();
        return false;
    }

    void removeRegisteredHotkey()
    {
        if (hotkeyRegistered)
        {
            UnregisterHotKey (window, kHotkeyId);
            hotkeyRegistered = false;
        }
    }

    bool addHook (const HotkeyBinding& b)
    {
        removeHook();

        g_hook.window.store (window, std::memory_order_relaxed);
        g_hook.vk.store (b.vk, std::memory_order_relaxed);
        g_hook.mods.store (b.mods, std::memory_order_relaxed);
        g_hook.passThrough.store (b.passThrough, std::memory_order_relaxed);
        g_hook.keyHeld.store (false, std::memory_order_relaxed);
        g_hook.readIndex = g_hook.writeIndex.load (std::memory_order_acquire);

        // グローバルな LL フックに専用 DLL は要らない（WH_KEYBOARD_LL の特例）。
        g_hookHandle = SetWindowsHookExW (WH_KEYBOARD_LL, &lowLevelKeyboardProc,
                                          GetModuleHandleW (nullptr), 0);

        hookInstalled = (g_hookHandle != nullptr);
        return hookInstalled;
    }

    void removeHook()
    {
        if (hookInstalled)
        {
            if (g_hookHandle != nullptr)
                UnhookWindowsHookEx (g_hookHandle);

            g_hookHandle = nullptr;
            hookInstalled = false;
        }

        g_hook.window.store (nullptr, std::memory_order_relaxed);
        g_hook.vk.store (0, std::memory_order_relaxed);
        g_hook.keyHeld.store (false, std::memory_order_relaxed);
    }

    //==========================================================================
    void drainHookEvents()
    {
        const uint32_t w = g_hook.writeIndex.load (std::memory_order_acquire);

        // 64 件を超えて溜まったぶんは捨てる（起こり得ないが、捨てる方が安全）。
        if (static_cast<uint32_t> (w - g_hook.readIndex) > HookShared::kQueueSize)
            g_hook.readIndex = w - HookShared::kQueueSize;

        while (g_hook.readIndex != w)
        {
            const uint8_t kind = g_hook.events[g_hook.readIndex & (HookShared::kQueueSize - 1)]
                                     .load (std::memory_order_relaxed);
            ++g_hook.readIndex;

            if (kind == HookShared::kEventDown)
                owner.listeners.call (&GlobalHotkey::Listener::hotkeyPressed);
            else if (kind == HookShared::kEventUp)
                owner.listeners.call (&GlobalHotkey::Listener::hotkeyReleased);
        }
    }

    void handleRegisteredHotkey()
    {
        owner.listeners.call (&GlobalHotkey::Listener::hotkeyPressed);
    }

    static LRESULT CALLBACK wndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_HOTKEY || msg == kMsgHotkeyEvent)
        {
            if (auto* self = reinterpret_cast<Impl*> (GetWindowLongPtrW (hwnd, GWLP_USERDATA)))
            {
                if (msg == WM_HOTKEY)
                    self->handleRegisteredHotkey();
                else
                    self->drainHookEvents();

                return 0;
            }
        }

        return DefWindowProcW (hwnd, msg, wParam, lParam);
    }

    //==========================================================================
    GlobalHotkey& owner;
    HWND window = nullptr;
    bool hotkeyRegistered = false;
    bool hookInstalled = false;

    int  tickCount = 0;
    int  missedDownPolls = 0;
    int  recoverCooldownTicks = 0;   // 30 秒。昇格ウィンドウ相手の誤検知で連呼しないため
};

//==============================================================================
GlobalHotkey::GlobalHotkey()
    : impl (std::make_unique<Impl> (*this))
{
}

GlobalHotkey::~GlobalHotkey()
{
    uninstall();
    impl.reset();
}

//==============================================================================
GlobalHotkey::BackendKind GlobalHotkey::chooseBackend (const HotkeyBinding& b) noexcept
{
    // ユーザーの要求からバックエンドを決める。逆はしない。
    // RegisterHotKey はキーを system-wide に飲み込むので、修飾キーがあり、
    // トグルで、パススルーが不要なときにしか使えない。
    if (b.mods != HotkeyMods::none
         && b.mode == HotkeyMode::toggle
         && ! b.passThrough)
        return BackendKind::registerHotKey;

    return BackendKind::lowLevelHook;
}

//==============================================================================
GlobalHotkey::InstallResult GlobalHotkey::install (const HotkeyBinding& newBinding)
{
    uninstall();

    if (isForbiddenBinding (newBinding))
        return InstallResult::invalidBinding;

    binding = newBinding;

    if (! impl->createWindow())
        return InstallResult::hookInstallFailed;

    const auto kind = chooseBackend (binding);

    if (kind == BackendKind::registerHotKey)
    {
        DWORD err = 0;

        if (! impl->addRegisteredHotkey (binding, err))
        {
            destroyMessageWindow (impl->window);

            return err == ERROR_HOTKEY_ALREADY_REGISTERED
                     ? InstallResult::alreadyRegisteredByAnotherApp
                     : InstallResult::hookInstallFailed;
        }
    }
    else
    {
        // ★ LL フックは Defender / EDR のキーロガーヒューリスティックに触れる。
        //    ユーザーの割当が本当に必要とするときだけ張る（既定の Ctrl+Shift+M では張らない）。
        if (! impl->addHook (binding))
        {
            destroyMessageWindow (impl->window);
            return InstallResult::hookInstallFailed;
        }
    }

    backend = kind;
    impl->tickCount = 0;
    impl->missedDownPolls = 0;

    // 照合タイマーは LL フックのときだけ必要。RegisterHotKey にキーアップは存在しない。
    if (backend == BackendKind::lowLevelHook)
        startTimer (kPollIntervalMs);

    return InstallResult::ok;
}

void GlobalHotkey::uninstall()
{
    stopTimer();

    if (impl != nullptr)
    {
        impl->removeHook();
        impl->removeRegisteredHotkey();
        destroyMessageWindow (impl->window);
        impl->missedDownPolls = 0;
    }

    backend = BackendKind::none;
}

//==============================================================================
bool GlobalHotkey::isKeyHeld() const noexcept
{
    if (backend != BackendKind::lowLevelHook)
        return false;

    return g_hook.keyHeld.load (std::memory_order_relaxed);
}

void GlobalHotkey::addListener (Listener* l)    { listeners.add (l); }
void GlobalHotkey::removeListener (Listener* l) { listeners.remove (l); }

//==============================================================================
void GlobalHotkey::timerCallback()
{
    if (backend != BackendKind::lowLevelHook)
        return;

    ++impl->tickCount;

    if (impl->recoverCooldownTicks > 0)
        --impl->recoverCooldownTicks;

    const bool physicallyDown = keyIsPhysicallyDown (static_cast<int> (binding.vk));
    const bool believedHeld   = g_hook.keyHeld.load (std::memory_order_relaxed);

    if (believedHeld && ! physicallyDown)
    {
        // 取りこぼしたキーアップ。昇格ウィンドウ・UAC・セッションロックでは
        // フックにキーアップが見えないので、押しっぱなしのまま固まる。
        g_hook.keyHeld.store (false, std::memory_order_relaxed);
        impl->missedDownPolls = 0;
        listeners.call (&Listener::hotkeyReleased);
        return;
    }

    if (! believedHeld && physicallyDown && modifiersMatchNow (binding.mods))
    {
        // フックが呼ばれていない疑い。Windows は遅いフックを黙って切るだけで
        // 通知もエラーも出さないため、「押されているのにイベントが来ない」が
        // 唯一の観測可能な死亡サイン。2 回連続（500 ms）で確定させる。
        // 昇格ウィンドウにフォーカスがある間も同じ症状（LL フックからは見えない）に
        // なるので、30 秒のクールダウンを挟んで再インストールを連呼しない。
        if (++impl->missedDownPolls >= 2 && impl->recoverCooldownTicks == 0)
        {
            impl->missedDownPolls = 0;
            impl->recoverCooldownTicks = 120;   // 250ms * 120 = 30 秒

            if (impl->addHook (binding))
                listeners.call (&Listener::hotkeyBackendRecovered);
        }

        return;
    }

    impl->missedDownPolls = 0;

    if (impl->tickCount >= kWatchdogTicks)
    {
        impl->tickCount = 0;

        // 念のためのハンドル健全性チェック。install 済みなのにハンドルが
        // 消えていたら張り直す。
        if (impl->hookInstalled && g_hookHandle == nullptr)
        {
            impl->hookInstalled = false;

            if (impl->addHook (binding))
                listeners.call (&Listener::hotkeyBackendRecovered);
        }
    }
}

//==============================================================================
juce::String GlobalHotkey::describeKey (uint32_t vk)
{
    // GetKeyNameTextW が当てにならない、あるいは生の英字名になってしまうものだけ
    // 先に潰す。0x1D をそのまま画面に出さないこと。
    switch (vk)
    {
        case SafeKeys::vkNonConvert: return jstr ("無変換");
        case SafeKeys::vkConvert:    return jstr ("変換");
        case VK_KANA:                return jstr ("かな");
        case VK_PAUSE:               return "Pause";      // MapVirtualKey が NumLock の SC を返す
        case VK_SNAPSHOT:            return "PrintScreen";
        case VK_SCROLL:              return "ScrollLock";
        case VK_SPACE:               return "Space";
        case VK_RETURN:              return "Enter";
        case VK_ESCAPE:              return "Esc";
        case VK_TAB:                 return "Tab";
        case VK_BACK:                return "BackSpace";
        default: break;
    }

    UINT scan = MapVirtualKeyW (vk, MAPVK_VK_TO_VSC_EX);

    if (scan == 0)
        scan = MapVirtualKeyW (vk, MAPVK_VK_TO_VSC);

    if (scan != 0)
    {
        LONG lp = static_cast<LONG> ((scan & 0xffu) << 16);

        const UINT prefix = (scan >> 8) & 0xffu;

        if (prefix == 0xe0 || prefix == 0xe1)
            lp |= (1 << 24);   // 拡張キーのビット

        wchar_t name[128] = {};

        // JIS 配列でも正しい名前が返る唯一の経路。
        if (GetKeyNameTextW (lp, name, static_cast<int> (juce::numElementsInArray (name))) > 0)
        {
            juce::String s (name);

            if (s.isNotEmpty())
                return s;
        }
    }

    if ((vk >= static_cast<uint32_t> ('A') && vk <= static_cast<uint32_t> ('Z'))
         || (vk >= static_cast<uint32_t> ('0') && vk <= static_cast<uint32_t> ('9')))
        return juce::String::charToString (static_cast<juce::juce_wchar> (vk));

    return juce::String ("0x") + juce::String::toHexString (static_cast<int> (vk)).toUpperCase();
}

juce::String GlobalHotkey::describeBinding (const HotkeyBinding& b)
{
    juce::String s;

    if ((b.mods & HotkeyMods::control) != 0) s << "Ctrl+";
    if ((b.mods & HotkeyMods::shift)   != 0) s << "Shift+";
    if ((b.mods & HotkeyMods::alt)     != 0) s << "Alt+";
    if ((b.mods & HotkeyMods::win)     != 0) s << "Win+";

    s << describeKey (b.vk);
    return s;
}

//==============================================================================
bool GlobalHotkey::isRiskyBareKey (const HotkeyBinding& b) noexcept
{
    if (b.mods != HotkeyMods::none)
        return false;

    const uint32_t vk = b.vk;

    const auto inRange = [vk] (uint32_t lo, uint32_t hi) { return vk >= lo && vk <= hi; };

    if (inRange ('A', 'Z')) return true;
    if (inRange ('0', '9')) return true;
    if (inRange (VK_NUMPAD0, VK_NUMPAD9)) return true;

    // OEM の記号キー群（JIS の @ [ ] : ; など）。0xBA..0xC0 と 0xDB..0xDF。
    if (inRange (VK_OEM_1, VK_OEM_3)) return true;
    if (inRange (VK_OEM_4, VK_OEM_8)) return true;
    if (vk == static_cast<uint32_t> (VK_OEM_102)) return true;

    return false;
}

bool GlobalHotkey::isForbiddenBinding (const HotkeyBinding& b) noexcept
{
    if (b.vk == 0)
        return true;

    // 修飾キー自体は割り当てさせない。修飾キーの完全一致判定と自己矛盾する。
    if (isModifierVk (b.vk))
        return true;

    // Esc はキャプチャのキャンセルに使うので常に予約。
    if (b.vk == VK_ESCAPE)
        return true;

    if (b.mods == HotkeyMods::none)
    {
        switch (b.vk)
        {
            case VK_RETURN:
            case VK_SPACE:
            case VK_TAB:
            case VK_BACK:
            case VK_DELETE:
                return true;
            default:
                break;
        }
    }

    return false;
}

//==============================================================================
juce::String GlobalHotkey::japaneseError (InstallResult r)
{
    switch (r)
    {
        case InstallResult::ok:
            return {};

        case InstallResult::alreadyRegisteredByAnotherApp:
            return jstr ("このキーは他のアプリが使用中です。別のキーを選んでください。");

        case InstallResult::hookInstallFailed:
            return jstr ("ショートカットキーを登録できませんでした。"
                       "セキュリティソフトがキー入力の監視をブロックしている可能性があります。");

        case InstallResult::invalidBinding:
            return jstr ("このキーは設定できません。別のキーを選んでください。");
    }

    return jstr ("ショートカットキーを登録できませんでした。");
}

//==============================================================================
//  HotkeyCapture::Impl
//==============================================================================
struct HotkeyCapture::Impl : private juce::Timer
{
    ~Impl() override { stop(); }

    bool start (Listener* l)
    {
        stop();

        if (l == nullptr)
            return false;

        listener = l;

        window = createMessageWindow (kCaptureWindowClass, &Impl::wndProc, this);

        if (window == nullptr)
        {
            listener = nullptr;
            return false;
        }

        g_capture.window.store (window, std::memory_order_relaxed);
        g_capture.vk.store (0, std::memory_order_relaxed);
        g_capture.mods.store (0, std::memory_order_relaxed);
        g_capture.cancelled.store (false, std::memory_order_relaxed);
        g_capture.pending.store (false, std::memory_order_relaxed);

        g_captureHook = SetWindowsHookExW (WH_KEYBOARD_LL, &captureKeyboardProc,
                                           GetModuleHandleW (nullptr), 0);

        if (g_captureHook == nullptr)
        {
            g_capture.window.store (nullptr, std::memory_order_relaxed);
            destroyMessageWindow (window);
            listener = nullptr;
            return false;
        }

        // 保険。フックが全打鍵を消費している状態でダイアログが落ちると
        // キーボードが system-wide に死ぬので、必ず自動で外れるようにする。
        startTimer (kCaptureSafetyMs);

        active = true;
        return true;
    }

    void stop()
    {
        stopTimer();

        if (g_captureHook != nullptr)
        {
            UnhookWindowsHookEx (g_captureHook);
            g_captureHook = nullptr;
        }

        g_capture.window.store (nullptr, std::memory_order_relaxed);
        g_capture.pending.store (false, std::memory_order_relaxed);

        destroyMessageWindow (window);

        listener = nullptr;
        active = false;
    }

    void handleCaptureEvent()
    {
        g_capture.pending.store (false, std::memory_order_relaxed);

        auto* l = listener;

        if (l == nullptr)
            return;

        if (g_capture.cancelled.exchange (false, std::memory_order_relaxed))
        {
            stop();
            l->captureCancelled();
            return;
        }

        const uint32_t vk   = g_capture.vk.exchange (0, std::memory_order_relaxed);
        const uint32_t mods = g_capture.mods.load (std::memory_order_relaxed);

        if (vk == 0)
            return;

        // フックを先に外してからコールバックする。コールバックの中で
        // 本番のバインドを install されてもフックが二重にならないように。
        stop();
        l->keyCaptured (vk, mods);
    }

    void timerCallback() override
    {
        auto* l = listener;
        stop();

        if (l != nullptr)
            l->captureCancelled();
    }

    static LRESULT CALLBACK wndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == kMsgCaptureEvent)
        {
            if (auto* self = reinterpret_cast<Impl*> (GetWindowLongPtrW (hwnd, GWLP_USERDATA)))
            {
                self->handleCaptureEvent();
                return 0;
            }
        }

        return DefWindowProcW (hwnd, msg, wParam, lParam);
    }

    HWND      window = nullptr;
    Listener* listener = nullptr;
    bool      active = false;
};

//==============================================================================
HotkeyCapture::HotkeyCapture()
    : impl (std::make_unique<Impl>())
{
}

HotkeyCapture::~HotkeyCapture()
{
    impl->stop();
    impl.reset();
}

bool HotkeyCapture::begin (Listener* l)    { return impl->start (l); }
void HotkeyCapture::end()                  { impl->stop(); }
bool HotkeyCapture::isActive() const noexcept { return impl != nullptr && impl->active; }

} // namespace kvc
